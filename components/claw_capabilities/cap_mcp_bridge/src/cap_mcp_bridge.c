/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_bridge.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cap_mcp_server.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_mcp_bridge";

#define CAP_MCP_BRIDGE_SOURCE   "mcp_bridge"
#define CAP_MCP_BRIDGE_CALL_BUF 4096
/* esp_mcp_tool_result_add_text() silently drops (not truncates) any text
 * over its internal CONFIG_MCP_TOOL_RESULT_TEXT_MAX_LEN (8192, not
 * Kconfig-exposed by this managed component) — a claw_list result over that
 * came back as an empty content array in live testing. Stay well under it. */
#define CAP_MCP_BRIDGE_LIST_TEXT_MAX 7500

/* See cap_mcp_bridge.h: SUB_AGENT is deliberate — cap_mcp_server's HTTP
 * transport is unauthenticated, so every request here is treated as the
 * least-trusted caller class, exactly like inbound MQTT commands. */
static claw_cap_call_context_t cap_mcp_bridge_context(void)
{
    claw_cap_call_context_t ctx = {0};
    ctx.caller = CLAW_CAP_CALLER_SUB_AGENT;
    ctx.source_cap = CAP_MCP_BRIDGE_SOURCE;
    return ctx;
}

/* Rebuilds claw_cap_build_llm_tools_json()'s array keeping only "name" (and
 * optionally "description") per entry, dropping the full input_schema —
 * that schema is what pushes the full catalog over CAP_MCP_BRIDGE_LIST_TEXT_MAX
 * once edge_agent's many cap_* groups are all registered. */
static char *cap_mcp_bridge_condense_tools(const char *full_tools_json, bool with_description)
{
    cJSON *full = cJSON_Parse(full_tools_json);
    if (!full || !cJSON_IsArray(full)) {
        cJSON_Delete(full);
        return NULL;
    }

    cJSON *light = cJSON_CreateArray();
    if (!light) {
        cJSON_Delete(full);
        return NULL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, full) {
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name)) {
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name->valuestring);
        if (with_description) {
            cJSON *desc = cJSON_GetObjectItem(item, "description");
            cJSON_AddStringToObject(entry, "description", cJSON_IsString(desc) ? desc->valuestring : "");
        }
        cJSON_AddItemToArray(light, entry);
    }
    cJSON_Delete(full);

    char *out = cJSON_PrintUnformatted(light);
    cJSON_Delete(light);
    return out;
}

static esp_mcp_value_t cap_mcp_bridge_list(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    claw_cap_call_context_t ctx = cap_mcp_bridge_context();
    char *tools_json = claw_cap_build_llm_tools_json(&ctx, false);
    if (!tools_json) {
        return esp_mcp_value_create_string("[]");
    }

    char *result = tools_json;
    char *condensed = NULL;
    if (strlen(tools_json) > CAP_MCP_BRIDGE_LIST_TEXT_MAX) {
        condensed = cap_mcp_bridge_condense_tools(tools_json, true);
        if (condensed && strlen(condensed) > CAP_MCP_BRIDGE_LIST_TEXT_MAX) {
            free(condensed);
            condensed = cap_mcp_bridge_condense_tools(tools_json, false);
        }
        if (condensed) {
            result = condensed;
        } else {
            ESP_LOGW(TAG, "claw_list: catalog too large to condense; result may be truncated by the MCP SDK");
        }
    }

    esp_mcp_value_t value = esp_mcp_value_create_string(result);
    free(tools_json);
    free(condensed);
    return value;
}

static esp_mcp_value_t cap_mcp_bridge_call(const esp_mcp_property_list_t *properties)
{
    const char *name = properties ? esp_mcp_property_list_get_property_string(properties, "name") : NULL;
    const char *args = properties ? esp_mcp_property_list_get_property_string(properties, "args") : NULL;

    if (!name || !name[0]) {
        return esp_mcp_value_create_string("Error: 'name' is required (see claw_list for available capabilities)");
    }

    char *out = calloc(1, CAP_MCP_BRIDGE_CALL_BUF);
    if (!out) {
        return esp_mcp_value_create_string("Error: out of memory");
    }

    claw_cap_call_context_t ctx = cap_mcp_bridge_context();
    claw_cap_call(name, (args && args[0]) ? args : "{}", &ctx, out, CAP_MCP_BRIDGE_CALL_BUF);

    esp_mcp_value_t value = esp_mcp_value_create_string(out);
    free(out);
    return value;
}

esp_err_t cap_mcp_bridge_init(void)
{
    static const cap_mcp_server_tool_def_t s_tools[] = {
        {
            .name = "claw_list",
            .description = "List claw_cap capabilities callable via claw_call, with their JSON input schema.",
            .callback = cap_mcp_bridge_list,
            .property_count = 0,
        },
        {
            .name = "claw_call",
            .description = "Call a claw_cap capability by name, as listed by claw_list.",
            .callback = cap_mcp_bridge_call,
            .property_names = {"name", "args"},
            .property_count = 2,
        },
    };

    esp_err_t err = cap_mcp_server_add_tool(s_tools, sizeof(s_tools) / sizeof(s_tools[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register bridge tools: %s", esp_err_to_name(err));
    }
    return err;
}
