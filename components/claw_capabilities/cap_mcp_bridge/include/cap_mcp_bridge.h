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

/*
 * cap_mcp_bridge: exposes claw_cap capabilities to MCP clients connected to
 * cap_mcp_server.
 *
 * The esp_mcp SDK's tool callback signature carries no per-tool userdata
 * (esp_mcp_tool_callback_t takes only the property list), so there is no way
 * to bind one static C callback per claw_cap capability without a large,
 * unmaintainable table of near-identical trampoline functions. Instead this
 * registers exactly two MCP tools that forward to claw_cap's own dispatch:
 *
 *   - claw_list: no arguments. Returns the JSON tool list claw_cap would
 *     hand a sub-agent (name, description, input schema per capability).
 *   - claw_call: { name, args } (args is a JSON object string, default
 *     "{}"). Forwards to claw_cap_call(name, args, ...) and returns its
 *     output as the tool result text.
 *
 * SECURITY: cap_mcp_server's HTTP transport has no authentication of its
 * own today (unlike cap_ssh, which is gated behind a public-key SSH
 * handshake). Both callbacks therefore dispatch with
 * CLAW_CAP_CALLER_SUB_AGENT — the least-trusted caller class — so
 * claw_cap_call() runs the full LLM-tool authorization path exactly as it
 * does for inbound MQTT commands (see cap_mqtt.c's cap_mqtt_run_capability()
 * for the precedent and the bug this avoids repeating): CALLABLE_BY_LLM is
 * required and ROOT_AGENT_ONLY/RESTRICTED capabilities are denied. Using
 * CLAW_CAP_CALLER_CONSOLE or ..._SYSTEM here would let any unauthenticated
 * network client invoke ROOT_AGENT_ONLY tools (ssh_configure,
 * wireguard_configure, ota_update, ...).
 */

/* Registers claw_list/claw_call with cap_mcp_server. Call after
 * cap_mcp_server_init() and before cap_mcp_server_start(). */
esp_err_t cap_mcp_bridge_init(void);

#ifdef __cplusplus
}
#endif
