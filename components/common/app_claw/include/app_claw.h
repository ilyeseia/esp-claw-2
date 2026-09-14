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

#define APP_CLAW_STR_LEN              320
#define APP_CLAW_SHORT_STR_LEN        32
#define APP_CLAW_MODEL_LEN            64
#define APP_CLAW_TIMEOUT_LEN          16
#define APP_CLAW_PATH_LEN             64
#define APP_CLAW_FILE_PATH_LEN        96

typedef struct claw_core_state *claw_core_handle_t;

typedef struct {
    char llm_api_key[APP_CLAW_STR_LEN];
    char llm_backend_type[APP_CLAW_SHORT_STR_LEN];
    char llm_model[APP_CLAW_MODEL_LEN];
    char llm_base_url[APP_CLAW_STR_LEN];
    char llm_auth_type[APP_CLAW_SHORT_STR_LEN];
    char llm_timeout_ms[APP_CLAW_TIMEOUT_LEN];
    char llm_max_tokens[APP_CLAW_TIMEOUT_LEN];
    char llm_default_image_max_bytes[APP_CLAW_TIMEOUT_LEN];
    char llm_max_tokens_field[APP_CLAW_SHORT_STR_LEN];
    char llm_supports_tools[8];
    char llm_supports_vision[8];
    char llm_image_remote_url_only[8];
    /* Optional fallback LLM backend: tried once when the primary backend
     * fails (e.g. rate-limited or down). Empty backend_type/base_url/model
     * disables it. Shares timeout/max_tokens/image/tool/vision knobs with
     * the primary backend. */
    char llm2_api_key[APP_CLAW_STR_LEN];
    char llm2_backend_type[APP_CLAW_SHORT_STR_LEN];
    char llm2_model[APP_CLAW_MODEL_LEN];
    char llm2_base_url[APP_CLAW_STR_LEN];
    char llm2_auth_type[APP_CLAW_SHORT_STR_LEN];
    char llm2_max_tokens_field[APP_CLAW_SHORT_STR_LEN];
    char qq_app_id[APP_CLAW_SHORT_STR_LEN];
    char qq_app_secret[APP_CLAW_STR_LEN];
    char qq_msg_type[8];
    char feishu_app_id[APP_CLAW_MODEL_LEN];
    char feishu_app_secret[APP_CLAW_STR_LEN];
    char tg_bot_token[APP_CLAW_STR_LEN];
    char wechat_token[APP_CLAW_STR_LEN];
    char wechat_base_url[APP_CLAW_STR_LEN];
    char wechat_cdn_base_url[APP_CLAW_STR_LEN];
    char wechat_account_id[APP_CLAW_SHORT_STR_LEN];
    char search_brave_key[APP_CLAW_STR_LEN];
    char search_tavily_key[APP_CLAW_STR_LEN];
    char search_http_allowlist[APP_CLAW_STR_LEN];
    char search_searxng_url[APP_CLAW_STR_LEN];
    char search_provider[16];
    char mqtt_enabled[8];
    char mqtt_broker[APP_CLAW_STR_LEN];
    char mqtt_port[16];
    char mqtt_tls_enabled[8];
    char mqtt_username[APP_CLAW_STR_LEN];
    char mqtt_password[APP_CLAW_STR_LEN];
    char mqtt_client_id[APP_CLAW_MODEL_LEN];
    char mqtt_keepalive[16];
    char mqtt_qos[16];
    char mqtt_base_topic[APP_CLAW_MODEL_LEN];
    char vpn_enabled[8];
    char vpn_gateway[APP_CLAW_STR_LEN];
    char vpn_test_host[APP_CLAW_STR_LEN];
    char vpn_test_port[16];
    char vpn_mode[24];
    char wg_private_key[64];
    char wg_address[48];
    char wg_peer_public_key[64];
    char wg_endpoint[128];
    char wg_endpoint_port[16];
    char wg_allowed_ips[128];
    char wg_keepalive[16];
    char wg_preshared_key[64];
    char wg_make_default[8];
    char net_use_static[8];
    char net_ip[20];
    char net_gateway[20];
    char net_netmask[20];
    char net_dns[20];
    char net_dns2[20];
    char asr_provider[APP_CLAW_SHORT_STR_LEN];
    char asr_api_key[APP_CLAW_STR_LEN];
    char asr_workspace_id[APP_CLAW_STR_LEN];
    char asr_language_hint[APP_CLAW_SHORT_STR_LEN];
    char asr_model[APP_CLAW_MODEL_LEN];
    char asr_endpoint[APP_CLAW_STR_LEN];
    char enabled_cap_groups[APP_CLAW_STR_LEN];
    char llm_visible_cap_groups[APP_CLAW_STR_LEN];
    char enabled_lua_modules[APP_CLAW_STR_LEN];
} app_claw_config_t;

typedef esp_err_t (*app_claw_save_config_fn)(const app_claw_config_t *config,
                                             void *user_ctx);

esp_err_t app_claw_set_save_config_callback(app_claw_save_config_fn save_config,
                                            void *user_ctx);
esp_err_t app_claw_start(const app_claw_config_t *config);
esp_err_t app_claw_update_config(const app_claw_config_t *config);
esp_err_t app_claw_get_config(app_claw_config_t *out_config);
esp_err_t app_claw_apply_config(const app_claw_config_t *config);
claw_core_handle_t app_claw_get_core(void);
esp_err_t app_claw_ui_start(void);
esp_err_t app_claw_set_network_status(bool sta_connected, const char *ap_ssid);

#ifdef __cplusplus
}
#endif
