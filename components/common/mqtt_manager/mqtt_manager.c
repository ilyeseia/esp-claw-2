/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MQTT transport wrapper around ESP-IDF's esp-mqtt client.
 *
 * Responsibilities (transport only; no agent/business logic):
 *   - own a single esp_mqtt_client and its lifecycle
 *   - derive a stable device id from the base MAC
 *   - compose the "{base}/{device_id}/..." topic namespace
 *   - publish a retained online/offline presence via connect + LWT
 *   - auto-subscribe to the command topic and forward inbound data to a callback
 *   - reconnect with exponential backoff (esp-mqtt auto-reconnect disabled)
 *
 * Inbound command routing (event router / core) is intentionally NOT done here:
 * a consumer registers an event callback and decides what to do, keeping this
 * component free of dependencies on the agent runtime.
 */
#include "mqtt_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "mqtt_manager";

#define MQTT_MANAGER_BACKOFF_BASE_MS  1000U
#define MQTT_MANAGER_BACKOFF_MAX_MS   60000U
#define MQTT_MANAGER_DEFAULT_PORT     1883
#define MQTT_MANAGER_DEFAULT_TLS_PORT 8883
#define MQTT_MANAGER_DEFAULT_KEEPALIVE 60
#define MQTT_MANAGER_DEFAULT_BASE     "espclaw"
#define MQTT_MANAGER_TOPIC_MAX        192
#define MQTT_MANAGER_URI_MAX          160

typedef struct mqtt_manager_ctx {
    esp_mqtt_client_handle_t client;
    SemaphoreHandle_t lock;
    esp_timer_handle_t reconnect_timer;
    uint32_t backoff_ms;

    char device_id[MQTT_MANAGER_DEVICE_ID_LEN];
    char base_topic[64];
    char broker_host[128];
    char command_topic[MQTT_MANAGER_TOPIC_MAX];
    char status_topic[MQTT_MANAGER_TOPIC_MAX];
    uint16_t port;
    uint8_t default_qos;
    bool tls_enabled;

    bool started;
    bool connected;
    uint32_t publish_count;
    uint32_t inbound_count;
    int last_error;

    mqtt_manager_event_cb_t event_cb;
    void *event_ctx;
} mqtt_manager_ctx;

static void mqtt_manager_derive_device_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        /* Fall back to a fixed id rather than leaving it empty. */
        strlcpy(out, "000000000000", out_size);
        return;
    }
    snprintf(out, out_size, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t mqtt_manager_build_topic(mqtt_manager_handle_t handle,
                                   const char *leaf,
                                   char *buf,
                                   size_t buf_size)
{
    int written;

    if (!handle || !leaf || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    written = snprintf(buf, buf_size, "%s/%s/%s", handle->base_topic, handle->device_id, leaf);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void mqtt_manager_arm_reconnect(mqtt_manager_ctx *ctx)
{
    if (!ctx->reconnect_timer) {
        return;
    }
    esp_timer_stop(ctx->reconnect_timer);
    esp_err_t err = esp_timer_start_once(ctx->reconnect_timer, (uint64_t)ctx->backoff_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm reconnect timer: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "MQTT reconnect scheduled in %u ms", (unsigned)ctx->backoff_ms);
    /* Grow the backoff for the next attempt, capped. */
    if (ctx->backoff_ms < MQTT_MANAGER_BACKOFF_MAX_MS) {
        ctx->backoff_ms *= 2;
        if (ctx->backoff_ms > MQTT_MANAGER_BACKOFF_MAX_MS) {
            ctx->backoff_ms = MQTT_MANAGER_BACKOFF_MAX_MS;
        }
    }
}

static void mqtt_manager_reconnect_timer_cb(void *arg)
{
    mqtt_manager_ctx *ctx = (mqtt_manager_ctx *)arg;

    if (!ctx || !ctx->client || !ctx->started) {
        return;
    }
    esp_err_t err = esp_mqtt_client_reconnect(ctx->client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mqtt_client_reconnect failed: %s", esp_err_to_name(err));
        mqtt_manager_arm_reconnect(ctx);
    }
}

static void mqtt_manager_on_connected(mqtt_manager_ctx *ctx)
{
    /* Reset backoff and (re)establish presence + command subscription. */
    if (xSemaphoreTake(ctx->lock, portMAX_DELAY) == pdTRUE) {
        ctx->connected = true;
        ctx->backoff_ms = MQTT_MANAGER_BACKOFF_BASE_MS;
        ctx->last_error = 0;
        xSemaphoreGive(ctx->lock);
    }

    esp_mqtt_client_publish(ctx->client, ctx->status_topic, "{\"online\":true}", 0, 1, true);
    esp_mqtt_client_subscribe(ctx->client, ctx->command_topic, ctx->default_qos);
    ESP_LOGI(TAG, "MQTT connected; subscribed to %s", ctx->command_topic);
}

static void mqtt_manager_dispatch(mqtt_manager_ctx *ctx, const mqtt_manager_event_t *event)
{
    if (ctx->event_cb) {
        ctx->event_cb(event, ctx->event_ctx);
    }
}

static void mqtt_manager_event_handler(void *handler_args,
                                       esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    mqtt_manager_ctx *ctx = (mqtt_manager_ctx *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    mqtt_manager_event_t out = {0};

    (void)base;
    if (!ctx || !event) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_manager_on_connected(ctx);
        out.id = MQTT_MANAGER_EVENT_CONNECTED;
        mqtt_manager_dispatch(ctx, &out);
        break;

    case MQTT_EVENT_DISCONNECTED:
        if (xSemaphoreTake(ctx->lock, portMAX_DELAY) == pdTRUE) {
            ctx->connected = false;
            xSemaphoreGive(ctx->lock);
        }
        ESP_LOGW(TAG, "MQTT disconnected");
        if (ctx->started) {
            mqtt_manager_arm_reconnect(ctx);
        }
        out.id = MQTT_MANAGER_EVENT_DISCONNECTED;
        mqtt_manager_dispatch(ctx, &out);
        break;

    case MQTT_EVENT_DATA:
        if (xSemaphoreTake(ctx->lock, portMAX_DELAY) == pdTRUE) {
            ctx->inbound_count++;
            xSemaphoreGive(ctx->lock);
        }
        out.id = MQTT_MANAGER_EVENT_DATA;
        out.topic = event->topic;
        out.topic_len = event->topic_len;
        out.data = event->data;
        out.data_len = event->data_len;
        mqtt_manager_dispatch(ctx, &out);
        break;

    case MQTT_EVENT_ERROR:
        if (xSemaphoreTake(ctx->lock, portMAX_DELAY) == pdTRUE) {
            if (event->error_handle) {
                ctx->last_error = event->error_handle->error_type;
            }
            xSemaphoreGive(ctx->lock);
        }
        ESP_LOGW(TAG, "MQTT error event");
        out.id = MQTT_MANAGER_EVENT_ERROR;
        mqtt_manager_dispatch(ctx, &out);
        break;

    default:
        break;
    }
}

esp_err_t mqtt_manager_create(const mqtt_manager_config_t *config, mqtt_manager_handle_t *ret_handle)
{
    mqtt_manager_ctx *ctx = NULL;
    esp_mqtt_client_config_t mqtt_cfg = {0};
    char uri[MQTT_MANAGER_URI_MAX];
    esp_err_t err = ESP_OK;

    if (!config || !config->broker || !config->broker[0] || !ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    mqtt_manager_derive_device_id(ctx->device_id, sizeof(ctx->device_id));
    strlcpy(ctx->base_topic,
            (config->base_topic && config->base_topic[0]) ? config->base_topic : MQTT_MANAGER_DEFAULT_BASE,
            sizeof(ctx->base_topic));
    strlcpy(ctx->broker_host, config->broker, sizeof(ctx->broker_host));
    ctx->tls_enabled = config->tls_enabled;
    ctx->default_qos = (config->default_qos == 1) ? 1 : 0;
    ctx->port = config->port ? config->port
                             : (config->tls_enabled ? MQTT_MANAGER_DEFAULT_TLS_PORT : MQTT_MANAGER_DEFAULT_PORT);
    ctx->backoff_ms = MQTT_MANAGER_BACKOFF_BASE_MS;

    (void)mqtt_manager_build_topic((mqtt_manager_handle_t)ctx, "command",
                                   ctx->command_topic, sizeof(ctx->command_topic));
    (void)mqtt_manager_build_topic((mqtt_manager_handle_t)ctx, "status",
                                   ctx->status_topic, sizeof(ctx->status_topic));

    snprintf(uri, sizeof(uri), "%s://%s", config->tls_enabled ? "mqtts" : "mqtt", ctx->broker_host);

    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.broker.address.port = ctx->port;
    mqtt_cfg.session.keepalive = config->keepalive ? config->keepalive : MQTT_MANAGER_DEFAULT_KEEPALIVE;
    mqtt_cfg.session.last_will.topic = ctx->status_topic;
    mqtt_cfg.session.last_will.msg = "{\"online\":false}";
    mqtt_cfg.session.last_will.msg_len = (int)strlen("{\"online\":false}");
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = true;
    /* Reconnect is driven by our exponential-backoff timer instead. */
    mqtt_cfg.network.disable_auto_reconnect = true;

    if (config->username && config->username[0]) {
        mqtt_cfg.credentials.username = config->username;
    }
    if (config->password && config->password[0]) {
        mqtt_cfg.credentials.authentication.password = config->password;
    }
    if (config->client_id && config->client_id[0]) {
        mqtt_cfg.credentials.client_id = config->client_id;
    } else {
        mqtt_cfg.credentials.client_id = ctx->device_id;
    }

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    if (config->tls_enabled) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    ctx->client = esp_mqtt_client_init(&mqtt_cfg);
    if (!ctx->client) {
        vSemaphoreDelete(ctx->lock);
        free(ctx);
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(ctx->client, ESP_EVENT_ANY_ID,
                                         mqtt_manager_event_handler, ctx);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(ctx->client);
        vSemaphoreDelete(ctx->lock);
        free(ctx);
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = mqtt_manager_reconnect_timer_cb,
        .arg = ctx,
        .name = "mqtt_reconnect",
    };
    err = esp_timer_create(&timer_args, &ctx->reconnect_timer);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(ctx->client);
        vSemaphoreDelete(ctx->lock);
        free(ctx);
        return err;
    }

    *ret_handle = ctx;
    ESP_LOGI(TAG, "MQTT manager created (device_id=%s broker=%s port=%u tls=%d)",
             ctx->device_id, ctx->broker_host, (unsigned)ctx->port, (int)ctx->tls_enabled);
    return ESP_OK;
}

esp_err_t mqtt_manager_start(mqtt_manager_handle_t handle)
{
    esp_err_t err;

    if (!handle || !handle->client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started) {
        return ESP_OK;
    }
    err = esp_mqtt_client_start(handle->client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        return err;
    }
    handle->started = true;
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(mqtt_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->started) {
        return ESP_OK;
    }
    if (handle->reconnect_timer) {
        esp_timer_stop(handle->reconnect_timer);
    }
    handle->started = false;
    /* Best-effort graceful offline notice before tearing down. */
    esp_mqtt_client_publish(handle->client, handle->status_topic, "{\"online\":false}", 0, 1, true);
    return esp_mqtt_client_stop(handle->client);
}

esp_err_t mqtt_manager_destroy(mqtt_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    mqtt_manager_stop(handle);
    if (handle->reconnect_timer) {
        esp_timer_delete(handle->reconnect_timer);
    }
    if (handle->client) {
        esp_mqtt_client_destroy(handle->client);
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
    return ESP_OK;
}

esp_err_t mqtt_manager_publish(mqtt_manager_handle_t handle,
                               const char *topic,
                               const char *payload,
                               int qos,
                               bool retain)
{
    int msg_id;

    if (!handle || !handle->client || !topic || !topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (qos < 0 || qos > 1) {
        qos = handle->default_qos;
    }
    msg_id = esp_mqtt_client_publish(handle->client, topic,
                                     payload ? payload : "",
                                     payload ? (int)strlen(payload) : 0,
                                     qos, retain);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        handle->publish_count++;
        xSemaphoreGive(handle->lock);
    }
    return ESP_OK;
}

esp_err_t mqtt_manager_publish_subtopic(mqtt_manager_handle_t handle,
                                        const char *leaf,
                                        const char *payload,
                                        int qos,
                                        bool retain)
{
    char topic[MQTT_MANAGER_TOPIC_MAX];
    esp_err_t err;

    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    err = mqtt_manager_build_topic(handle, leaf, topic, sizeof(topic));
    if (err != ESP_OK) {
        return err;
    }
    return mqtt_manager_publish(handle, topic, payload, qos, retain);
}

esp_err_t mqtt_manager_subscribe(mqtt_manager_handle_t handle, const char *topic, int qos)
{
    int msg_id;

    if (!handle || !handle->client || !topic || !topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (qos < 0 || qos > 1) {
        qos = handle->default_qos;
    }
    msg_id = esp_mqtt_client_subscribe(handle->client, topic, qos);
    return (msg_id < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_manager_get_status(mqtt_manager_handle_t handle, mqtt_manager_status_t *out_status)
{
    if (!handle || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        out_status->started = handle->started;
        out_status->connected = handle->connected;
        strlcpy(out_status->device_id, handle->device_id, sizeof(out_status->device_id));
        strlcpy(out_status->broker, handle->broker_host, sizeof(out_status->broker));
        out_status->port = handle->port;
        out_status->tls_enabled = handle->tls_enabled;
        out_status->publish_count = handle->publish_count;
        out_status->inbound_count = handle->inbound_count;
        out_status->last_error = handle->last_error;
        xSemaphoreGive(handle->lock);
    }
    return ESP_OK;
}

const char *mqtt_manager_get_device_id(mqtt_manager_handle_t handle)
{
    return handle ? handle->device_id : NULL;
}

esp_err_t mqtt_manager_register_event_cb(mqtt_manager_handle_t handle,
                                         mqtt_manager_event_cb_t cb,
                                         void *user_ctx)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) == pdTRUE) {
        handle->event_cb = cb;
        handle->event_ctx = user_ctx;
        xSemaphoreGive(handle->lock);
    }
    return ESP_OK;
}
