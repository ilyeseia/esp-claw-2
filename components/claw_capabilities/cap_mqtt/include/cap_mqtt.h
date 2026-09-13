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

/* Registers the cap_mqtt group (mqtt_publish, mqtt_status, mqtt_subscribe). */
esp_err_t cap_mqtt_register_group(void);

#ifdef __cplusplus
}
#endif
