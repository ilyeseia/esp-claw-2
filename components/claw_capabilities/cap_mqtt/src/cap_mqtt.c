/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cap_mqtt: model-callable MQTT tools + inbound command bridge, built on top of
 * mqtt_manager.
 *
 * Tools:
 *   - mqtt_publish      : publish a payload to a leaf under this device's subtree
 *   - mqtt_status       : report connection state and counters (never secrets)
 *   - mqtt_subscribe    : subscribe to a leaf under this device's subtree (restricted)
 *   - mqtt_send_message : outbound sender bound to the "mqtt" channel; the event
 *                         router calls it to deliver agent replies to the response
 *                         topic. Also LLM-callable.
 *
 * Inbound command bridge:
 *   mqtt_manager auto-subscribes to "{base}/{device_id}/command" and forwards
 *   inbound data to cap_mqtt_on_event (esp-mqtt task). To keep that task
 *   responsive, payloads are copied and queued to a worker task which:
 *     - action == "capability": runs claw_cap_call and publishes the result to
 *       the response topic;
 *     - otherwise: routes the text into claw_event_router as a "mqtt" channel
 *       message, so the agent handles it and replies via mqtt_send_message.
 *
 * All tool topics are confined to "{base}/{device_id}/<leaf>". Credentials are
 * never echoed.
 */
#include "cap_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "mqtt_manager.h"

static const char *TAG = "cap_mqtt";

#define CAP_MQTT_CHANNEL          "mqtt"
#define CAP_MQTT_SOURCE           "mqtt_gateway"
#define CAP_MQTT_SENDER           "mqtt_user"
#define CAP_MQTT_CMD_SUFFIX       "/command"
#define CAP_MQTT_CMD_QUEUE_LEN    8
#define CAP_MQTT_CMD_MAX_BYTES    2048
#define CAP_MQTT_RESP_BUF         2048
#define CAP_MQTT_WORKER_STACK     6144
#define CAP_MQTT_CHAT_ID_LEN      64

typedef struct {
    mqtt_manager_handle_t mqtt;
    QueueHandle_t cmd_queue;   /* items: char * (heap payload, NUL-terminated) */
    TaskHandle_t worker;
    cap_mqtt_persist_fn persist;
    void *persist_ctx;
} cap_mqtt_state_t;

static cap_mqtt_state_t s_state = {0};

esp_err_t cap_mqtt_set_persist_provider(cap_mqtt_persist_fn persist, void *user_ctx)
{
    s_state.persist = persist;
    s_state.persist_ctx = user_ctx;
    return ESP_OK;
}

static const char *cap_mqtt_json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

/* Rejects leaves that would break out of the device subtree or use wildcards. */
static bool cap_mqtt_leaf_is_valid(const char *leaf)
{
    if (!leaf || !leaf[0] || leaf[0] == '/') {
        return false;
    }
    for (const char *p = leaf; *p; p++) {
        if (*p == '#' || *p == '+' || (unsigned char)*p < 0x20) {
            return false;
        }
    }
    return true;
}

static void cap_mqtt_publish_response(const char *chat_id, const char *capability, bool ok, const char *result)
{
    cJSON *root = NULL;
    char *serialized = NULL;

    if (!s_state.mqtt) {
        return;
    }
    root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "id", chat_id ? chat_id : "");
    if (capability) {
        cJSON_AddStringToObject(root, "capability", capability);
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    if (result) {
        cJSON_AddStringToObject(root, "result", result);
    }
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized) {
        mqtt_manager_publish_subtopic(s_state.mqtt, "response", serialized, 1, false);
        cJSON_free(serialized);
    }
}

static void cap_mqtt_run_capability(const char *chat_id, const char *cap_name, cJSON *input_item)
{
    char *input_str = NULL;
    char *out = NULL;

    if (input_item) {
        input_str = cJSON_PrintUnformatted(input_item);
    }
    out = calloc(1, CAP_MQTT_RESP_BUF);
    if (!out) {
        cap_mqtt_publish_response(chat_id, cap_name, false, "out of memory");
        if (input_str) {
            cJSON_free(input_str);
        }
        return;
    }

    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .channel = CAP_MQTT_CHANNEL,
        .chat_id = chat_id,
        .source_cap = CAP_MQTT_SOURCE,
    };
    esp_err_t err = claw_cap_call(cap_name, input_str ? input_str : "{}", &ctx, out, CAP_MQTT_RESP_BUF);
    cap_mqtt_publish_response(chat_id, cap_name, err == ESP_OK, out);

    free(out);
    if (input_str) {
        cJSON_free(input_str);
    }
}

static void cap_mqtt_process_command(const char *payload)
{
    cJSON *root = NULL;
    const char *action = NULL;
    const char *text = NULL;
    const char *id = NULL;
    const char *cap_name = NULL;
    cJSON *cap_input = NULL;
    char chat_id[CAP_MQTT_CHAT_ID_LEN] = CAP_MQTT_CHANNEL;

    root = cJSON_Parse(payload);
    if (root) {
        action = cap_mqtt_json_string(root, "action");
        id = cap_mqtt_json_string(root, "id");
        text = cap_mqtt_json_string(root, "text");
        if (!text) {
            text = cap_mqtt_json_string(root, "message");
        }
        cap_name = cap_mqtt_json_string(root, "capability");
        if (!cap_name) {
            cap_name = cap_mqtt_json_string(root, "name");
        }
        cap_input = cJSON_GetObjectItem(root, "input");
        if (id && id[0]) {
            strlcpy(chat_id, id, sizeof(chat_id));
        }
    }

    if (action && strcmp(action, "capability") == 0 && cap_name && cap_name[0]) {
        ESP_LOGI(TAG, "MQTT command: capability '%s' (id=%s)", cap_name, chat_id);
        cap_mqtt_run_capability(chat_id, cap_name, cap_input);
    } else {
        /* Route free text (or the raw payload) to the agent via the event
         * router; the reply comes back through mqtt_send_message. */
        const char *msg_text = text ? text : (root ? NULL : payload);
        if (msg_text && msg_text[0]) {
            esp_err_t err = claw_event_router_publish_message(CAP_MQTT_SOURCE,
                                                             CAP_MQTT_CHANNEL,
                                                             chat_id,
                                                             msg_text,
                                                             CAP_MQTT_SENDER,
                                                             (id && id[0]) ? id : NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to route MQTT message: %s", esp_err_to_name(err));
                cap_mqtt_publish_response(chat_id, NULL, false, "failed to route message");
            }
        } else {
            ESP_LOGW(TAG, "Ignoring MQTT command with no text/action");
        }
    }

    if (root) {
        cJSON_Delete(root);
    }
}

static void cap_mqtt_worker(void *arg)
{
    char *payload = NULL;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_state.cmd_queue, &payload, portMAX_DELAY) == pdTRUE && payload) {
            cap_mqtt_process_command(payload);
            free(payload);
            payload = NULL;
        }
    }
}

static void cap_mqtt_on_event(const mqtt_manager_event_t *event, void *user_ctx)
{
    static const char suffix[] = CAP_MQTT_CMD_SUFFIX;
    const size_t suffix_len = sizeof(suffix) - 1;
    char *copy = NULL;

    (void)user_ctx;
    if (!event || event->id != MQTT_MANAGER_EVENT_DATA || !s_state.cmd_queue) {
        return;
    }
    /* Only handle the command topic; ignore other subscriptions. */
    if (event->topic_len < suffix_len ||
            memcmp(event->topic + event->topic_len - suffix_len, suffix, suffix_len) != 0) {
        return;
    }
    if (event->data_len == 0 || event->data_len > CAP_MQTT_CMD_MAX_BYTES) {
        return;
    }

    copy = malloc(event->data_len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, event->data, event->data_len);
    copy[event->data_len] = '\0';

    if (xQueueSend(s_state.cmd_queue, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full; dropping inbound message");
        free(copy);
    }
}

static esp_err_t cap_mqtt_ensure_worker(void)
{
    if (!s_state.cmd_queue) {
        s_state.cmd_queue = xQueueCreate(CAP_MQTT_CMD_QUEUE_LEN, sizeof(char *));
        if (!s_state.cmd_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_state.worker) {
        if (xTaskCreate(cap_mqtt_worker, "mqtt_cmd", CAP_MQTT_WORKER_STACK, NULL, 5, &s_state.worker) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t cap_mqtt_set_config(const cap_mqtt_config_t *config)
{
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Tear down any existing transport so new settings take effect cleanly. */
    if (s_state.mqtt) {
        mqtt_manager_destroy(s_state.mqtt);
        s_state.mqtt = NULL;
    }

    if (!config->enabled || !config->broker || !config->broker[0]) {
        ESP_LOGI(TAG, "MQTT disabled or no broker configured; transport not started");
        return ESP_OK;
    }

    mqtt_manager_config_t mgr_cfg = {
        .broker = config->broker,
        .port = config->port,
        .tls_enabled = config->tls_enabled,
        .username = config->username,
        .password = config->password,
        .client_id = config->client_id,
        .keepalive = config->keepalive,
        .default_qos = config->qos,
        .base_topic = config->base_topic,
    };

    err = mqtt_manager_create(&mgr_cfg, &s_state.mqtt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MQTT manager: %s", esp_err_to_name(err));
        s_state.mqtt = NULL;
        return err;
    }

    err = cap_mqtt_ensure_worker();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT command worker: %s", esp_err_to_name(err));
        mqtt_manager_destroy(s_state.mqtt);
        s_state.mqtt = NULL;
        return err;
    }

    mqtt_manager_register_event_cb(s_state.mqtt, cap_mqtt_on_event, NULL);

    err = mqtt_manager_start(s_state.mqtt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT manager: %s", esp_err_to_name(err));
        mqtt_manager_destroy(s_state.mqtt);
        s_state.mqtt = NULL;
        return err;
    }

    return ESP_OK;
}

static esp_err_t cap_mqtt_publish_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *input = NULL;
    cJSON *topic = NULL;
    cJSON *payload = NULL;
    cJSON *qos = NULL;
    cJSON *retain = NULL;
    int qos_val = 0;
    bool retain_val = false;
    char full_topic[192];
    esp_err_t err;

    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.mqtt) {
        snprintf(output, output_size, "Error: MQTT is not configured or disabled");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    topic = cJSON_GetObjectItem(input, "topic");
    payload = cJSON_GetObjectItem(input, "payload");
    qos = cJSON_GetObjectItem(input, "qos");
    retain = cJSON_GetObjectItem(input, "retain");

    if (!cJSON_IsString(topic) || !cap_mqtt_leaf_is_valid(topic->valuestring)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: 'topic' must be a sub-topic name (no leading '/', no wildcards)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsString(payload)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'payload' must be a string");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(qos)) {
        qos_val = (qos->valueint == 1) ? 1 : 0;
    }
    if (cJSON_IsBool(retain)) {
        retain_val = cJSON_IsTrue(retain);
    }

    err = mqtt_manager_build_topic(s_state.mqtt, topic->valuestring, full_topic, sizeof(full_topic));
    if (err != ESP_OK) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: topic too long");
        return err;
    }

    err = mqtt_manager_publish(s_state.mqtt, full_topic, payload->valuestring, qos_val, retain_val);
    cJSON_Delete(input);

    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(output, output_size, "Error: MQTT not connected");
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: publish failed (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "Published to %s (qos=%d retain=%s)",
             full_topic, qos_val, retain_val ? "true" : "false");
    return ESP_OK;
}

static esp_err_t cap_mqtt_status_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    mqtt_manager_status_t status = {0};

    (void)input_json;
    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.mqtt) {
        snprintf(output, output_size, "MQTT: disabled (not configured)");
        return ESP_OK;
    }

    mqtt_manager_get_status(s_state.mqtt, &status);
    snprintf(output, output_size,
             "MQTT status:\n"
             "  state: %s\n"
             "  broker: %s:%u (tls=%s)\n"
             "  device_id: %s\n"
             "  published: %u, inbound: %u\n"
             "  last_error: %d",
             status.connected ? "connected" : (status.started ? "connecting" : "stopped"),
             status.broker, (unsigned)status.port, status.tls_enabled ? "yes" : "no",
             status.device_id,
             (unsigned)status.publish_count, (unsigned)status.inbound_count,
             status.last_error);
    return ESP_OK;
}

static esp_err_t cap_mqtt_subscribe_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *input = NULL;
    cJSON *topic = NULL;
    cJSON *qos = NULL;
    int qos_val = 0;
    char full_topic[192];
    esp_err_t err;

    (void)ctx;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.mqtt) {
        snprintf(output, output_size, "Error: MQTT is not configured or disabled");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    topic = cJSON_GetObjectItem(input, "topic");
    qos = cJSON_GetObjectItem(input, "qos");
    if (!cJSON_IsString(topic) || !cap_mqtt_leaf_is_valid(topic->valuestring)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: 'topic' must be a sub-topic name (no leading '/', no wildcards)");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(qos)) {
        qos_val = (qos->valueint == 1) ? 1 : 0;
    }

    err = mqtt_manager_build_topic(s_state.mqtt, topic->valuestring, full_topic, sizeof(full_topic));
    if (err != ESP_OK) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: topic too long");
        return err;
    }

    err = mqtt_manager_subscribe(s_state.mqtt, full_topic, qos_val);
    cJSON_Delete(input);

    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(output, output_size, "Error: MQTT not connected");
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: subscribe failed (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "Subscribed to %s (qos=%d)", full_topic, qos_val);
    return ESP_OK;
}

static esp_err_t cap_mqtt_send_message_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *input = NULL;
    const char *chat_id = NULL;
    const char *message = NULL;
    cJSON *resp = NULL;
    char *serialized = NULL;
    esp_err_t err = ESP_FAIL;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.mqtt) {
        snprintf(output, output_size, "Error: MQTT is not configured or disabled");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json ? input_json : "{}");
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    chat_id = cap_mqtt_json_string(input, "chat_id");
    message = cap_mqtt_json_string(input, "message");
    if ((!chat_id || !chat_id[0]) && ctx && ctx->chat_id && ctx->chat_id[0]) {
        chat_id = ctx->chat_id;
    }
    if (!message || !message[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'message' is required");
        return ESP_ERR_INVALID_ARG;
    }

    resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddStringToObject(resp, "chat_id", chat_id ? chat_id : "");
        cJSON_AddStringToObject(resp, "message", message);
        serialized = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
    }

    if (serialized) {
        err = mqtt_manager_publish_subtopic(s_state.mqtt, "response", serialized, 1, false);
        cJSON_free(serialized);
    }
    cJSON_Delete(input);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: publish failed (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "reply sent to MQTT response topic");
    return ESP_OK;
}

static int cap_mqtt_json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return atoi(item->valuestring);
    }
    return fallback;
}

static bool cap_mqtt_json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
    }
    return fallback;
}

/*
 * mqtt_configure: set the broker connection from a chat/tool call, apply it live
 * and (if a persist provider is wired) save it to NVS so it survives reboot.
 * Root-agent only + restricted: changing the broker/credentials is sensitive, so
 * sub-agents (which may be handling untrusted content) are not allowed to call it.
 * The password is never echoed back in the result.
 */
static esp_err_t cap_mqtt_configure_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)ctx;

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *broker = cap_mqtt_json_string(input, "broker");
    if (!broker || !broker[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'broker' is required");
        return ESP_ERR_INVALID_ARG;
    }

    bool tls = cap_mqtt_json_bool(input, "tls", false);
    bool enabled = cap_mqtt_json_bool(input, "enabled", true);
    int port = cap_mqtt_json_int(input, "port", tls ? 8883 : 1883);
    int keepalive = cap_mqtt_json_int(input, "keepalive", 60);
    int qos = cap_mqtt_json_int(input, "qos", 0);

    const char *username = cap_mqtt_json_string(input, "username");
    const char *password = cap_mqtt_json_string(input, "password");
    const char *client_id = cap_mqtt_json_string(input, "client_id");
    const char *base_topic = cap_mqtt_json_string(input, "base_topic");

    /* Copy into local buffers; set_config/mqtt_manager copy them into the handle. */
    char broker_buf[128];
    char user_buf[128];
    char pass_buf[192];
    char client_buf[64];
    char topic_buf[64];
    strlcpy(broker_buf, broker, sizeof(broker_buf));
    strlcpy(user_buf, username ? username : "", sizeof(user_buf));
    strlcpy(pass_buf, password ? password : "", sizeof(pass_buf));
    strlcpy(client_buf, client_id ? client_id : "", sizeof(client_buf));
    strlcpy(topic_buf, (base_topic && base_topic[0]) ? base_topic : "espclaw", sizeof(topic_buf));
    cJSON_Delete(input);

    if (port < 1 || port > 65535) {
        snprintf(output, output_size, "Error: port must be between 1 and 65535");
        return ESP_ERR_INVALID_ARG;
    }
    if (qos < 0 || qos > 1) {
        qos = 0;
    }
    if (keepalive < 1) {
        keepalive = 60;
    }

    cap_mqtt_config_t cfg = {
        .enabled = enabled,
        .broker = broker_buf,
        .port = (uint16_t)port,
        .tls_enabled = tls,
        .username = user_buf,
        .password = pass_buf,
        .client_id = client_buf,
        .keepalive = (uint16_t)keepalive,
        .qos = (uint8_t)qos,
        .base_topic = topic_buf,
    };

    esp_err_t err = cap_mqtt_set_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to apply MQTT config (%s)", esp_err_to_name(err));
        return err;
    }

    bool persisted = false;
    if (s_state.persist) {
        esp_err_t perr = s_state.persist(&cfg, s_state.persist_ctx);
        persisted = (perr == ESP_OK);
        if (!persisted) {
            ESP_LOGW(TAG, "MQTT config applied live but persist failed: %s", esp_err_to_name(perr));
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "broker", broker_buf);
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddBoolToObject(root, "tls", tls);
    cJSON_AddBoolToObject(root, "auth", user_buf[0] != '\0');
    cJSON_AddBoolToObject(root, "persisted", persisted);
    cJSON_AddStringToObject(root, "note",
                            enabled ? "Applied live; connecting in the background — check mqtt_status."
                                    : "MQTT disabled.");
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(output, output_size, "Error: failed to encode result");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_mqtt_descriptors[] = {
    {
        .id = "mqtt_publish",
        .name = "mqtt_publish",
        .family = "network",
        .description = "Publish a payload to a sub-topic under this device's MQTT namespace.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"topic\":{\"type\":\"string\",\"description\":\"sub-topic leaf (no leading slash or wildcards)\"},"
        "\"payload\":{\"type\":\"string\"},"
        "\"qos\":{\"type\":\"integer\",\"enum\":[0,1]},"
        "\"retain\":{\"type\":\"boolean\"}},"
        "\"required\":[\"topic\",\"payload\"]}",
        .execute = cap_mqtt_publish_execute,
    },
    {
        .id = "mqtt_status",
        .name = "mqtt_status",
        .family = "network",
        .description = "Report MQTT connection state, broker host, device id, and counters.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_mqtt_status_execute,
    },
    {
        .id = "mqtt_subscribe",
        .name = "mqtt_subscribe",
        .family = "network",
        .description = "Subscribe to a sub-topic under this device's MQTT namespace.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"topic\":{\"type\":\"string\",\"description\":\"sub-topic leaf (no leading slash or wildcards)\"},"
        "\"qos\":{\"type\":\"integer\",\"enum\":[0,1]}},"
        "\"required\":[\"topic\"]}",
        .execute = cap_mqtt_subscribe_execute,
    },
    {
        .id = "mqtt_send_message",
        .name = "mqtt_send_message",
        .family = "network",
        .description = "Send a text reply to the MQTT response topic (outbound channel binding).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"channel\":{\"type\":\"string\"},"
        "\"chat_id\":{\"type\":\"string\"},"
        "\"message\":{\"type\":\"string\"}},"
        "\"required\":[\"message\"]}",
        .execute = cap_mqtt_send_message_execute,
    },
    {
        .id = "mqtt_configure",
        .name = "mqtt_configure",
        .family = "network",
        .description = "Configure the MQTT broker connection (broker/port/TLS/credentials/base_topic), "
                       "apply it live and persist it across reboots. Restricted, root-agent only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"broker\":{\"type\":\"string\",\"description\":\"host or IP, no scheme\"},"
        "\"port\":{\"type\":\"integer\"},"
        "\"tls\":{\"type\":\"boolean\"},"
        "\"username\":{\"type\":\"string\"},"
        "\"password\":{\"type\":\"string\"},"
        "\"client_id\":{\"type\":\"string\"},"
        "\"keepalive\":{\"type\":\"integer\"},"
        "\"qos\":{\"type\":\"integer\",\"enum\":[0,1]},"
        "\"base_topic\":{\"type\":\"string\"},"
        "\"enabled\":{\"type\":\"boolean\"}},"
        "\"required\":[\"broker\"]}",
        .execute = cap_mqtt_configure_execute,
    },
};

static const claw_cap_group_t s_mqtt_group = {
    .group_id = "cap_mqtt",
    .descriptors = s_mqtt_descriptors,
    .descriptor_count = sizeof(s_mqtt_descriptors) / sizeof(s_mqtt_descriptors[0]),
};

esp_err_t cap_mqtt_register_group(void)
{
    if (claw_cap_group_exists(s_mqtt_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_mqtt_group);
}
