/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cap_home_assistant: talks to a Home Assistant instance's real REST API
 * (https://developers.home-assistant.io/docs/api/rest/) with a long-lived access token, exposed as
 * four claw_cap tools:
 *   - ha_get_entities  : GET  /api/states                    (optionally filtered by domain)
 *   - ha_get_state     : GET  /api/states/<entity_id>
 *   - ha_call_service  : POST /api/services/<domain>/<service> with a JSON body
 *   - ha_search_entity : substring search over ha_get_entities' result
 *
 * Deliberately NOT modelled on the reference implementation (espclaw-homeAssistante): that project
 * (a) requires the operator to pre-register a full URL per entity instead of using HA's real
 * discovery endpoint, (b) fetches every configured sensor synchronously before every single LLM
 * turn (see docs/multi-agent/architecture_report.md §2.3, finding R5) rather than on demand through
 * the existing tool-calling loop, and (c) disables TLS certificate validation unconditionally, even
 * on ESP32, and (d) logs the bearer token as part of a debug curl trace. None of that is repeated
 * here: entity/domain/service names supplied by the model are validated against Home Assistant's
 * own identifier charset before being placed in a URL path (never taken as a full URL), requests
 * use `esp_crt_bundle_attach` for real certificate validation whenever `https://` is configured,
 * every call has an explicit timeout, and the token is never logged.
 */

/* Registers the cap_home_assistant group. */
esp_err_t cap_home_assistant_register_group(void);

/* enabled: mirrors the "ha_enabled" config toggle — tools refuse to run while false.
 * base_url: e.g. "http://homeassistant.local:8123" (no trailing slash, no /api suffix).
 * token: a Home Assistant long-lived access token. Copied internally; never logged. */
esp_err_t cap_home_assistant_set_config(bool enabled, const char *base_url, const char *token);

#ifdef __cplusplus
}
#endif
