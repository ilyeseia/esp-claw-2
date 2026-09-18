/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_core.h"
#include "llm/claw_llm_runtime.h"

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
} claw_core_llm_config_t;

typedef claw_llm_tool_call_t claw_core_llm_tool_call_t;
typedef claw_llm_response_t claw_core_llm_response_t;

/* Device-wide (all claw_core instances: root agent + any subagents) running
 * totals of LLM token usage, accumulated from every chat_messages() call
 * whose backend reported a "usage" object (OpenAI-compatible and Anthropic
 * both do; a custom backend that doesn't just isn't counted in the token
 * totals — see request_count_unknown_usage).
 * Raw token counts only, deliberately not converted to a $ estimate: actual
 * per-token pricing is provider- and model-specific and changes over time,
 * so a hardcoded conversion here would silently go stale. */
typedef struct {
    uint32_t request_count_with_usage;
    uint32_t request_count_unknown_usage;
    uint64_t total_prompt_tokens;
    uint64_t total_completion_tokens;
    uint64_t total_tokens;
} claw_core_llm_usage_totals_t;

void claw_core_llm_get_usage_totals(claw_core_llm_usage_totals_t *out_totals);
void claw_core_llm_reset_usage_totals(void);

esp_err_t claw_core_llm_init(const claw_core_llm_config_t *config,
                             claw_llm_runtime_t **out_runtime,
                             char **out_error_message);
esp_err_t claw_core_llm_chat_messages(claw_core_handle_t core,
                                      const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json,
                                      claw_core_llm_response_t *out_response,
                                      char **out_error_message);
esp_err_t claw_core_llm_infer_media(claw_core_handle_t core,
                                    const claw_llm_media_request_t *request,
                                    char **out_text,
                                    char **out_error_message);
esp_err_t claw_core_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration);
void claw_core_llm_response_free(claw_core_llm_response_t *response);
