/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_platform.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"

static const char *TAG = "cap_platform";

#define CAP_PLATFORM_NVS_NAMESPACE   "cap_platform"
#define CAP_PLATFORM_NVS_KEY_SECRET  "secret"

#define CAP_PLATFORM_SECRET_MIN_LEN  16   /* 128-bit minimum */
#define CAP_PLATFORM_SECRET_MAX_LEN  64   /* 512-bit ceiling, plenty */
#define CAP_PLATFORM_NONCE_LEN_MAX   48
#define CAP_PLATFORM_NONCE_WINDOW    16
#define CAP_PLATFORM_MAX_TOKEN_TTL_S 60
#define CAP_PLATFORM_CLOCK_SKEW_S    5

/* Matches cap_system.c's own CAP_SYSTEM_MIN_VALID_EPOCH sentinel (2024-01-01):
 * before SNTP sync, time(NULL) reads near the 1970 epoch, which would make
 * every token look either "already expired" or, worse, satisfy a forged
 * expires_at of 0 — reject outright instead of trusting an unset clock. */
#define CAP_PLATFORM_MIN_VALID_EPOCH 1704067200

typedef struct {
    SemaphoreHandle_t lock;
    uint8_t secret[CAP_PLATFORM_SECRET_MAX_LEN];
    size_t secret_len;
    bool secret_set;
    char seen_nonces[CAP_PLATFORM_NONCE_WINDOW][CAP_PLATFORM_NONCE_LEN_MAX];
    size_t nonce_next;
    size_t nonce_count;
} cap_platform_state_t;

static cap_platform_state_t s_platform;

static const char *cap_platform_json_str(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

static double cap_platform_json_num(cJSON *root, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static void cap_platform_get_device_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};

    /* Same derivation as mqtt_manager_derive_device_id() (mqtt_manager.c) —
     * duplicated rather than shared because it's six lines and pulling this
     * component into mqtt_manager's dependency graph just for that would be
     * a worse coupling than the duplication. */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        strlcpy(out, "000000000000", out_size);
        return;
    }
    snprintf(out, out_size, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Accepts base64url (RFC 4648 §5: '-'/'_' , no padding) by translating to
 * standard base64 and padding before handing off to mbedtls. */
static esp_err_t cap_platform_base64url_decode(const char *in, size_t in_len,
                                               uint8_t *out, size_t out_cap,
                                               size_t *out_len)
{
    char buf[700];
    size_t pad;

    if (!in || !out || !out_len || in_len == 0 || in_len > 512) {
        return ESP_ERR_INVALID_ARG;
    }
    pad = (4 - (in_len % 4)) % 4;
    if (in_len + pad >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        }
        buf[i] = c;
    }
    for (size_t i = 0; i < pad; i++) {
        buf[in_len + i] = '=';
    }

    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(out, out_cap, &decoded_len,
                                    (const unsigned char *)buf, in_len + pad);
    if (ret != 0) {
        return ESP_FAIL;
    }
    *out_len = decoded_len;
    return ESP_OK;
}

static esp_err_t cap_platform_hmac_sha256(const uint8_t *key, size_t key_len,
                                          const uint8_t *msg, size_t msg_len,
                                          uint8_t out[32])
{
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) {
        return ESP_FAIL;
    }
    if (mbedtls_md_hmac(md_info, key, key_len, msg, msg_len, out) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Constant-time so a mismatching byte position can't leak via timing. */
static bool cap_platform_constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

/* Bounded in-RAM replay window — the device has no persistent nonce store,
 * so the <=60s TTL enforced below is the primary defense; this is a
 * secondary one, sufficient because the window only needs to outlive one
 * TTL's worth of traffic, not the device's lifetime. Must be called with
 * s_platform.lock held. */
static bool cap_platform_nonce_replayed_locked(const char *nonce)
{
    if (!nonce || !nonce[0]) {
        return true; /* no nonce at all is treated as unsafe, not "unused" */
    }
    for (size_t i = 0; i < s_platform.nonce_count; i++) {
        if (strcmp(s_platform.seen_nonces[i], nonce) == 0) {
            return true;
        }
    }
    strlcpy(s_platform.seen_nonces[s_platform.nonce_next], nonce,
            sizeof(s_platform.seen_nonces[0]));
    s_platform.nonce_next = (s_platform.nonce_next + 1) % CAP_PLATFORM_NONCE_WINDOW;
    if (s_platform.nonce_count < CAP_PLATFORM_NONCE_WINDOW) {
        s_platform.nonce_count++;
    }
    return false;
}

static esp_err_t cap_platform_persist_secret(const uint8_t *secret, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CAP_PLATFORM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, CAP_PLATFORM_NVS_KEY_SECRET, secret, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void cap_platform_load_secret(void)
{
    nvs_handle_t handle;
    if (nvs_open(CAP_PLATFORM_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return; /* not yet provisioned; platform_exec reports that clearly */
    }
    size_t len = sizeof(s_platform.secret);
    if (nvs_get_blob(handle, CAP_PLATFORM_NVS_KEY_SECRET, s_platform.secret, &len) == ESP_OK &&
            len >= CAP_PLATFORM_SECRET_MIN_LEN) {
        s_platform.secret_len = len;
        s_platform.secret_set = true;
        ESP_LOGI(TAG, "Loaded platform secret from NVS (%u bytes)", (unsigned)len);
    }
    nvs_close(handle);
}

static esp_err_t cap_platform_group_init(void)
{
    if (!s_platform.lock) {
        s_platform.lock = xSemaphoreCreateMutex();
        if (!s_platform.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    cap_platform_load_secret();
    return ESP_OK;
}

/*
 * platform_configure: set the shared HMAC secret. Root-agent only, so it is
 * NOT reachable via the MQTT command bridge (cap_mqtt.c calls claw_cap_call
 * with CLAW_CAP_CALLER_SUB_AGENT, and claw_cap_authorize_llm_tool_locked()
 * denies ROOT_AGENT_ONLY tools for that caller) — only local/trusted
 * channels (console, WebIM/Telegram as the device owner) can set it, by
 * design. The secret is a base64url-encoded value the operator generates
 * platform-side; this device never invents one for itself.
 */
static esp_err_t cap_platform_configure_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)ctx;

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *secret_b64 = cap_platform_json_str(input, "secret");
    if (!secret_b64 || !secret_b64[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: 'secret' is required (base64url, decoding to >= %u bytes)",
                 (unsigned)CAP_PLATFORM_SECRET_MIN_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t decoded[CAP_PLATFORM_SECRET_MAX_LEN];
    size_t decoded_len = 0;
    esp_err_t err = cap_platform_base64url_decode(secret_b64, strlen(secret_b64),
                                                  decoded, sizeof(decoded), &decoded_len);
    cJSON_Delete(input);
    if (err != ESP_OK || decoded_len < CAP_PLATFORM_SECRET_MIN_LEN) {
        snprintf(output, output_size,
                 "Error: secret must be valid base64url decoding to >= %u bytes",
                 (unsigned)CAP_PLATFORM_SECRET_MIN_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_platform_persist_secret(decoded, decoded_len);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to persist secret (%s)", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_platform.lock, portMAX_DELAY);
    memcpy(s_platform.secret, decoded, decoded_len);
    s_platform.secret_len = decoded_len;
    s_platform.secret_set = true;
    s_platform.nonce_count = 0;
    s_platform.nonce_next = 0;
    xSemaphoreGive(s_platform.lock);

    ESP_LOGI(TAG, "Platform secret set (%u bytes)", (unsigned)decoded_len);
    snprintf(output, output_size,
             "{\"ok\":true,\"note\":\"Platform secret set (%u bytes). Never echoed back.\"}",
             (unsigned)decoded_len);
    return ESP_OK;
}

/*
 * platform_exec: verify a signed token, then invoke its target capability
 * internally with root-agent authority. See cap_platform.h for the full
 * token shape and rationale. Every failure path returns a specific reason —
 * deliberately, so a legitimate integration bug (clock skew, stale secret)
 * is distinguishable from an actual forged/replayed token in logs.
 */
static esp_err_t cap_platform_exec_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_platform.lock, portMAX_DELAY);
    bool have_secret = s_platform.secret_set;
    uint8_t secret_copy[CAP_PLATFORM_SECRET_MAX_LEN];
    size_t secret_len = s_platform.secret_len;
    if (have_secret) {
        memcpy(secret_copy, s_platform.secret, secret_len);
    }
    xSemaphoreGive(s_platform.lock);

    if (!have_secret) {
        snprintf(output, output_size,
                 "Error: platform secret not configured (run platform_configure locally first)");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *outer = cJSON_Parse(input_json);
    if (!outer) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *token = cap_platform_json_str(outer, "token");
    if (!token || !token[0]) {
        cJSON_Delete(outer);
        snprintf(output, output_size, "Error: 'token' is required");
        return ESP_ERR_INVALID_ARG;
    }

    const char *dot = strchr(token, '.');
    if (!dot || dot == token || !dot[1]) {
        cJSON_Delete(outer);
        snprintf(output, output_size, "Error: malformed token (expected <payload>.<sig>)");
        return ESP_ERR_INVALID_ARG;
    }
    size_t payload_b64_len = (size_t)(dot - token);
    const char *sig_b64 = dot + 1;

    /* 1. Verify the signature over the ASCII base64url payload segment
     *    (JWT-style — see the header doc comment for why). */
    uint8_t sig_bytes[64] = {0};
    size_t sig_len = 0;
    if (cap_platform_base64url_decode(sig_b64, strlen(sig_b64), sig_bytes, sizeof(sig_bytes), &sig_len) != ESP_OK ||
            sig_len != 32) {
        cJSON_Delete(outer);
        snprintf(output, output_size, "Error: malformed token signature");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t computed_hmac[32];
    if (cap_platform_hmac_sha256(secret_copy, secret_len,
                                 (const uint8_t *)token, payload_b64_len,
                                 computed_hmac) != ESP_OK) {
        cJSON_Delete(outer);
        snprintf(output, output_size, "Error: signature computation failed");
        return ESP_FAIL;
    }
    if (!cap_platform_constant_time_eq(computed_hmac, sig_bytes, 32)) {
        ESP_LOGW(TAG, "platform_exec: signature mismatch (forged or stale-secret token)");
        cJSON_Delete(outer);
        snprintf(output, output_size, "Error: invalid token signature");
        return ESP_ERR_INVALID_ARG;
    }

    /* 2. Only now decode+parse the payload — nothing below this point is
     *    trusted until the signature above already proved it wasn't tampered with. */
    uint8_t payload_bytes[1024];
    size_t payload_len = 0;
    if (cap_platform_base64url_decode(token, payload_b64_len, payload_bytes,
                                      sizeof(payload_bytes) - 1, &payload_len) != ESP_OK) {
        cJSON_Delete(outer);
        snprintf(output, output_size, "Error: malformed token payload encoding");
        return ESP_ERR_INVALID_ARG;
    }
    payload_bytes[payload_len] = '\0';

    cJSON *payload = cJSON_Parse((const char *)payload_bytes);
    cJSON_Delete(outer);
    if (!payload) {
        snprintf(output, output_size, "Error: token payload is not valid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = cap_platform_json_str(payload, "device_id");
    const char *capability = cap_platform_json_str(payload, "capability");
    const char *nonce = cap_platform_json_str(payload, "nonce");
    double issued_at = cap_platform_json_num(payload, "issued_at", -1);
    double expires_at = cap_platform_json_num(payload, "expires_at", -1);
    cJSON *target_input = cJSON_GetObjectItem(payload, "input");

    char own_device_id[16];
    cap_platform_get_device_id(own_device_id, sizeof(own_device_id));

    if (!device_id || strcmp(device_id, own_device_id) != 0) {
        ESP_LOGW(TAG, "platform_exec: token device_id mismatch (own=%s)", own_device_id);
        cJSON_Delete(payload);
        snprintf(output, output_size, "Error: token was issued for a different device");
        return ESP_ERR_INVALID_ARG;
    }
    if (!capability || !capability[0]) {
        cJSON_Delete(payload);
        snprintf(output, output_size, "Error: token missing target capability");
        return ESP_ERR_INVALID_ARG;
    }
    if (issued_at < 0 || expires_at < 0 || expires_at <= issued_at ||
            (expires_at - issued_at) > CAP_PLATFORM_MAX_TOKEN_TTL_S) {
        cJSON_Delete(payload);
        snprintf(output, output_size,
                 "Error: invalid token validity window (must be issued_at < expires_at, TTL <= %ds)",
                 CAP_PLATFORM_MAX_TOKEN_TTL_S);
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    if (now < CAP_PLATFORM_MIN_VALID_EPOCH) {
        cJSON_Delete(payload);
        snprintf(output, output_size, "Error: system time not set; cannot verify token expiry");
        return ESP_ERR_INVALID_STATE;
    }
    if ((double)now < issued_at - CAP_PLATFORM_CLOCK_SKEW_S || (double)now > expires_at) {
        cJSON_Delete(payload);
        snprintf(output, output_size, "Error: token expired or not yet valid");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_platform.lock, portMAX_DELAY);
    bool replayed = cap_platform_nonce_replayed_locked(nonce);
    xSemaphoreGive(s_platform.lock);
    if (replayed) {
        ESP_LOGW(TAG, "platform_exec: nonce replay rejected for capability=%s", capability);
        cJSON_Delete(payload);
        snprintf(output, output_size, "Error: token nonce already used (replay)");
        return ESP_ERR_INVALID_ARG;
    }

    /* 3. All checks passed — invoke the real target with root-agent
     *    authority. This is the one place in the codebase that deliberately
     *    escalates a SUB_AGENT-originated call to CLAW_CAP_CALLER_ROOT_AGENT,
     *    and it only does so after the signature/expiry/replay/device_id
     *    checks above — never unconditionally. */
    char *target_input_str = target_input ? cJSON_PrintUnformatted(target_input) : NULL;
    claw_cap_call_context_t inner_ctx = {
        .caller = CLAW_CAP_CALLER_ROOT_AGENT,
        .source_cap = "cap_platform",
    };
    esp_err_t ret = claw_cap_call(capability, target_input_str ? target_input_str : "{}",
                                  &inner_ctx, output, output_size);

    free(target_input_str);
    cJSON_Delete(payload);
    return ret;
}

static const claw_cap_descriptor_t s_platform_descriptors[] = {
    {
        .id = "platform_configure",
        .name = "platform_configure",
        .family = "system",
        .description = "Set the shared secret used to verify platform_exec tokens (base64url, "
                       "decoding to >= 16 bytes). Root-agent only — not reachable over MQTT. "
                       "Overwrites any existing secret; never echoed back.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"secret\":{\"type\":\"string\",\"description\":\"base64url-encoded shared secret\"}},"
        "\"required\":[\"secret\"]}",
        .execute = cap_platform_configure_execute,
    },
    {
        .id = "platform_exec",
        .name = "platform_exec",
        .family = "system",
        .description = "Execute a signed, time-limited platform command token against a restricted "
                       "capability. Verifies an HMAC-SHA256 token (device-bound, single-use, <=60s "
                       "TTL) before invoking the target capability with root-agent authority. "
                       "Reachable over the MQTT command bridge, unlike ROOT_AGENT_ONLY tools, "
                       "because authorization here comes from the signed token, not the caller.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"token\":{\"type\":\"string\",\"description\":\"signed platform command token\"}},"
        "\"required\":[\"token\"]}",
        .execute = cap_platform_exec_execute,
    },
};

static const claw_cap_group_t s_platform_group = {
    .group_id = "cap_platform",
    .descriptors = s_platform_descriptors,
    .descriptor_count = sizeof(s_platform_descriptors) / sizeof(s_platform_descriptors[0]),
    .group_init = cap_platform_group_init,
};

esp_err_t cap_platform_register_group(void)
{
    if (claw_cap_group_exists(s_platform_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_platform_group);
}
