/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    const char *broker;
    uint16_t port;
    bool tls_enabled;
    const char *username;
    const char *password;
    const char *client_id;
    uint16_t keepalive;
    uint8_t qos;
    const char *base_topic;
} cap_mqtt_config_t;

/*
 * Applies broker configuration and (re)builds the underlying transport.
 * When config->enabled is false or no broker is set, the transport is torn
 * down and the tools report a disabled/unconfigured state. Safe to call again
 * to apply new settings.
 */
esp_err_t cap_mqtt_set_config(const cap_mqtt_config_t *config);

/*
 * Optional persistence hook. When set, the (root-agent-only) mqtt_configure tool
 * writes the applied settings through this callback so they survive a reboot.
 * The app wires this to its NVS-backed config store. Without it, mqtt_configure
 * still applies settings live but they are lost on restart.
 */
typedef esp_err_t (*cap_mqtt_persist_fn)(const cap_mqtt_config_t *config, void *user_ctx);
esp_err_t cap_mqtt_set_persist_provider(cap_mqtt_persist_fn persist, void *user_ctx);

/* Registers the cap_mqtt group (mqtt_publish, mqtt_status, mqtt_subscribe,
 * mqtt_send_message, mqtt_configure). */
esp_err_t cap_mqtt_register_group(void);

#ifdef __cplusplus
}
#endif
