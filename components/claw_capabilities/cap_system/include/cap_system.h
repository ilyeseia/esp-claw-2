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

typedef bool (*cap_system_network_ready_fn)(void *ctx);
typedef void (*cap_system_sync_success_fn)(bool had_valid_time, void *ctx);

typedef struct {
    cap_system_network_ready_fn network_ready;
    void *network_ready_ctx;
    cap_system_sync_success_fn on_sync_success;
    void *on_sync_success_ctx;
    uint32_t disconnected_retry_ms;
    uint32_t sync_retry_ms;
} cap_system_time_sync_service_config_t;

/* Persists a POSIX TZ string so it survives a reboot. Wired by the application
 * to its NVS-backed config store; the set_timezone tool applies the value live
 * and then calls this to save it. */
typedef esp_err_t (*cap_system_timezone_persist_fn)(const char *timezone, void *ctx);

esp_err_t cap_system_register_group(void);
esp_err_t cap_system_time_sync_service_start(const cap_system_time_sync_service_config_t *config);
esp_err_t cap_system_set_timezone_persist_provider(cap_system_timezone_persist_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
