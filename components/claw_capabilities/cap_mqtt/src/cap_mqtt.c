/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cap_mqtt: model-callable MQTT tools built on top of mqtt_manager.
 *
 * Tools:
 *   - mqtt_publish   : publish a payload to a leaf under this device's subtree
 *   - mqtt_status    : report connection state and counters (never secrets)
 *   - mqtt_subscribe : subscribe to a leaf under this device's subtree (restricted)
 *
 * All tool topics are confined to "{base}/{device_id}/<leaf>" so the model can
 * never address arbitrary brokers/topics. Credentials are never echoed.
 */
#include "cap_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"

#include "mqtt_manager.h"

static const char *TAG = "cap_mqtt";

typedef struct {
    mqtt_manager_handle_t mqtt;
} cap_mqtt_state_t;

static cap_mqtt_state_t s_state = {0};

/* Rejects leaves that would break out of the device subtree or use wildcards. */
static bool cap_mqtt_leaf_is_valid(const char *leaf)
{
    if (!leaf || !leaf[0]) {
        return false;
    }
    if (leaf[0] == '/') {
        return false;
    }
    for (const char *p = leaf; *p; p++) {
        if (*p == '#' || *p == '+') {
            return false;
        }
        if ((unsigned char)*p < 0x20) {
            return false;
        }
    }
    return true;
}

esp_err_t cap_mqtt_set_config(const cap_mqtt_config_t *config)
{
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

    esp_err_t err = mqtt_manager_create(&mgr_cfg, &s_state.mqtt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MQTT manager: %s", esp_err_to_name(err));
        s_state.mqtt = NULL;
        return err;
    }

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
