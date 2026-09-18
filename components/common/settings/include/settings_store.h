/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *namespace_name;
} settings_store_config_t;

typedef struct {
    const char *key;
    const char *value;
} settings_store_kv_t;

esp_err_t settings_store_init(const settings_store_config_t *config);
esp_err_t settings_store_get_string(const char *key,
                                    char *buf,
                                    size_t buf_size,
                                    const char *default_value);
esp_err_t settings_store_has_key(const char *key, bool *exists);
esp_err_t settings_store_set_string(const char *key, const char *value);
/*
 * Writes several string values under a single NVS open/commit/close instead
 * of one per key — use this for saving a whole config object at once (e.g.
 * app_config_save()'s ~80 fields) rather than looping settings_store_set_string(),
 * which pays a full flash commit per call even when most values are
 * unchanged. All-or-nothing: on the first write error, the handle is closed
 * without committing, so nothing from this batch is persisted.
 */
esp_err_t settings_store_set_strings(const settings_store_kv_t *items, size_t count);
esp_err_t settings_store_erase_key(const char *key);
esp_err_t settings_store_commit(void);

#ifdef __cplusplus
}
#endif
