/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_web_search_register_group(void);
esp_err_t cap_web_search_set_brave_key(const char *api_key);
esp_err_t cap_web_search_set_tavily_key(const char *api_key);
/* Base URL of a self-hosted SearXNG instance (e.g. "http://host" or "https://host"). */
esp_err_t cap_web_search_set_searxng_url(const char *url);
/* Provider selector: "auto" | "brave" | "tavily" | "searxng" (empty == auto). */
esp_err_t cap_web_search_set_provider(const char *provider);

#ifdef __cplusplus
}
#endif
