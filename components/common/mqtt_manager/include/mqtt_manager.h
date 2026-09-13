/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Length of the MAC-derived device id string (12 hex chars + NUL). */
#define MQTT_MANAGER_DEVICE_ID_LEN 13

typedef struct mqtt_manager_ctx *mqtt_manager_handle_t;

typedef enum {
    MQTT_MANAGER_EVENT_CONNECTED = 0,
    MQTT_MANAGER_EVENT_DISCONNECTED = 1,
    MQTT_MANAGER_EVENT_DATA = 2,
    MQTT_MANAGER_EVENT_ERROR = 3,
} mqtt_manager_event_id_t;

typedef struct {
    mqtt_manager_event_id_t id;
    const char *topic;      /* valid for DATA; not NUL-terminated, use topic_len */
    size_t topic_len;
    const char *data;       /* valid for DATA; not NUL-terminated, use data_len */
    size_t data_len;
} mqtt_manager_event_t;

typedef void (*mqtt_manager_event_cb_t)(const mqtt_manager_event_t *event, void *user_ctx);

typedef struct {
    const char *broker;         /* bare host or IP (no scheme, no port) */
    uint16_t port;              /* 0 -> 1883 (TCP) or 8883 (TLS) */
    bool tls_enabled;
    const char *username;       /* empty/NULL -> anonymous */
    const char *password;       /* empty/NULL -> none */
    const char *client_id;      /* empty/NULL -> derived from device id */
    uint16_t keepalive;         /* 0 -> 60 s */
    uint8_t default_qos;        /* 0 or 1 */
    const char *base_topic;     /* empty/NULL -> "espclaw" */
} mqtt_manager_config_t;

typedef struct {
    bool started;
    bool connected;
    char device_id[MQTT_MANAGER_DEVICE_ID_LEN];
    char broker[128];           /* host only; never carries credentials */
    uint16_t port;
    bool tls_enabled;
    uint32_t publish_count;
    uint32_t inbound_count;
    int last_error;             /* esp_err_t / esp-mqtt error, 0 = none */
} mqtt_manager_status_t;

/* Lifecycle -------------------------------------------------------------- */
esp_err_t mqtt_manager_create(const mqtt_manager_config_t *config, mqtt_manager_handle_t *ret_handle);
esp_err_t mqtt_manager_start(mqtt_manager_handle_t handle);
esp_err_t mqtt_manager_stop(mqtt_manager_handle_t handle);
esp_err_t mqtt_manager_destroy(mqtt_manager_handle_t handle);

/* Messaging -------------------------------------------------------------- */
/* Publishes to an absolute topic. */
esp_err_t mqtt_manager_publish(mqtt_manager_handle_t handle,
                               const char *topic,
                               const char *payload,
                               int qos,
                               bool retain);
/* Publishes to "{base}/{device_id}/{leaf}". */
esp_err_t mqtt_manager_publish_subtopic(mqtt_manager_handle_t handle,
                                        const char *leaf,
                                        const char *payload,
                                        int qos,
                                        bool retain);
esp_err_t mqtt_manager_subscribe(mqtt_manager_handle_t handle, const char *topic, int qos);

/* Introspection ---------------------------------------------------------- */
esp_err_t mqtt_manager_get_status(mqtt_manager_handle_t handle, mqtt_manager_status_t *out_status);
const char *mqtt_manager_get_device_id(mqtt_manager_handle_t handle);
/* Builds "{base}/{device_id}/{leaf}" into buf. */
esp_err_t mqtt_manager_build_topic(mqtt_manager_handle_t handle,
                                   const char *leaf,
                                   char *buf,
                                   size_t buf_size);
esp_err_t mqtt_manager_register_event_cb(mqtt_manager_handle_t handle,
                                         mqtt_manager_event_cb_t cb,
                                         void *user_ctx);

#ifdef __cplusplus
}
#endif
