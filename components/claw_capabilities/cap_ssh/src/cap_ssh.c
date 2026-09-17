/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_ssh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

/* Matches the ESP-IDF wolfssh_echoserver reference example's main.c include
 * order exactly (not the generic cross-platform echoserver.c header block,
 * whose #else branch expects a wolfssl/options.h this managed component
 * doesn't generate) — settings.h pulls in the managed component's own
 * user_settings.h. */
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssh/ssh.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "cap_ssh";

#define CAP_SSH_PORT            22
#define CAP_SSH_HOSTKEY_MAX     2800  /* decoded DER; covers RSA 4096 (~2470B DER) and ECDSA with margin */
#define CAP_SSH_HOSTKEY_B64_MAX 3800  /* base64 of CAP_SSH_HOSTKEY_MAX (3734B) plus room to spare —
                                       * app_config persists this as an NVS string, capped at 4000
                                       * bytes including the NUL terminator, so this must stay under it */
#define CAP_SSH_AUTHKEY_MAX     600   /* decoded SSH wire pubkey blob */
#define CAP_SSH_AUTHKEY_LINE_MAX 1024 /* full "type base64 comment" OpenSSH line */
#define CAP_SSH_LINE_MAX        1024
#define CAP_SSH_RESP_MAX        (16 * 1024)
#define CAP_SSH_SERVER_STACK    16384 /* wolfSSH/wolfCrypt handshake is stack-heavy; tune after live testing */

/* v1: one session at a time, public-key auth only, a small purpose-built
 * shell (list/call/groups) rather than the full esp_console REPL. See
 * cap_ssh.h for the full rationale, including the app_config-backed
 * configuration flow (host_key_b64/authorized_public_key are kept verbatim
 * alongside the decoded forms purely so cap_ssh_persist_current() can hand
 * the original strings back to the persist provider unchanged). */
typedef struct {
    SemaphoreHandle_t lock;
    bool enabled;
    char host_key_b64[CAP_SSH_HOSTKEY_B64_MAX];
    char authorized_public_key[CAP_SSH_AUTHKEY_LINE_MAX];
    uint8_t hostkey[CAP_SSH_HOSTKEY_MAX]; /* decoded DER */
    size_t hostkey_len;
    uint8_t authkey[CAP_SSH_AUTHKEY_MAX]; /* decoded SSH wire pubkey blob */
    size_t authkey_len;
    bool configured;
    TaskHandle_t server_task;
} cap_ssh_state_t;

static cap_ssh_state_t s_ssh;
static cap_ssh_persist_fn s_persist;
static void *s_persist_ctx;

static esp_err_t cap_ssh_start_server(void);

static const char *cap_ssh_json_str(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

/* Parses a standard OpenSSH public key line ("ssh-ed25519 AAAA... comment")
 * and returns the decoded wire-format key blob — the same bytes wolfSSH
 * hands the user-auth callback in WS_UserAuthData.sf.publicKey.publicKey,
 * confirmed against wolfSSH's own LoadPublicKeyBuffer() reference
 * implementation (examples/echoserver/main/echoserver.c). */
static esp_err_t cap_ssh_parse_authorized_key(const char *line, uint8_t *out, size_t out_cap, size_t *out_len)
{
    const char *type_end = strchr(line, ' ');
    if (!type_end) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *b64 = type_end + 1;
    while (*b64 == ' ') {
        b64++;
    }
    const char *b64_end = strchr(b64, ' ');
    size_t b64_len = b64_end ? (size_t)(b64_end - b64) : strlen(b64);
    if (b64_len == 0 || b64_len > 900) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(out, out_cap, &decoded_len, (const unsigned char *)b64, b64_len);
    if (ret != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = decoded_len;
    return ESP_OK;
}

static bool cap_ssh_constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static esp_err_t cap_ssh_ensure_lock(void)
{
    if (!s_ssh.lock) {
        s_ssh.lock = xSemaphoreCreateMutex();
        if (!s_ssh.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t cap_ssh_set_persist_provider(cap_ssh_persist_fn persist, void *user_ctx)
{
    s_persist = persist;
    s_persist_ctx = user_ctx;
    return ESP_OK;
}

/* Persist current state through the app-provided hook, if any. Mirrors
 * cap_vpn's cap_vpn_persist_current(). */
static bool cap_ssh_persist_current(void)
{
    if (!s_persist) {
        return false;
    }
    xSemaphoreTake(s_ssh.lock, portMAX_DELAY);
    cap_ssh_config_t cfg = {
        .enabled = s_ssh.enabled,
        .host_private_key_der_b64 = s_ssh.host_key_b64,
        .authorized_public_key = s_ssh.authorized_public_key,
    };
    xSemaphoreGive(s_ssh.lock);
    esp_err_t err = s_persist(&cfg, s_persist_ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSH persist failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

esp_err_t cap_ssh_set_config(const cap_ssh_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = cap_ssh_ensure_lock();
    if (err != ESP_OK) {
        return err;
    }

    bool have_host = config->host_private_key_der_b64 && config->host_private_key_der_b64[0];
    bool have_auth = config->authorized_public_key && config->authorized_public_key[0];
    bool should_start = false;

    xSemaphoreTake(s_ssh.lock, portMAX_DELAY);
    s_ssh.enabled = config->enabled;

    if (have_host && have_auth) {
        uint8_t hostkey_buf[CAP_SSH_HOSTKEY_MAX];
        size_t hostkey_len = 0;
        int b64ret = mbedtls_base64_decode(hostkey_buf, sizeof(hostkey_buf), &hostkey_len,
                                           (const unsigned char *)config->host_private_key_der_b64,
                                           strlen(config->host_private_key_der_b64));

        uint8_t authkey_buf[CAP_SSH_AUTHKEY_MAX];
        size_t authkey_len = 0;
        esp_err_t parse_err = (b64ret == 0) ?
            cap_ssh_parse_authorized_key(config->authorized_public_key, authkey_buf,
                                         sizeof(authkey_buf), &authkey_len) :
            ESP_ERR_INVALID_ARG;

        if (b64ret != 0 || parse_err != ESP_OK) {
            xSemaphoreGive(s_ssh.lock);
            return ESP_ERR_INVALID_ARG;
        }

        memcpy(s_ssh.hostkey, hostkey_buf, hostkey_len);
        s_ssh.hostkey_len = hostkey_len;
        memcpy(s_ssh.authkey, authkey_buf, authkey_len);
        s_ssh.authkey_len = authkey_len;
        strlcpy(s_ssh.host_key_b64, config->host_private_key_der_b64, sizeof(s_ssh.host_key_b64));
        strlcpy(s_ssh.authorized_public_key, config->authorized_public_key,
                sizeof(s_ssh.authorized_public_key));
        s_ssh.configured = true;
    }
    should_start = s_ssh.enabled && s_ssh.configured;
    xSemaphoreGive(s_ssh.lock);

    return should_start ? cap_ssh_start_server() : ESP_OK;
}

/*
 * Public-key-only auth. Reachable only after a real SSH handshake, so the
 * offered key is already proven possession-of-private-key by wolfSSH before
 * this callback runs — we only need to check it matches the one authorized
 * key, in constant time. Any other auth type (password, none) is rejected
 * outright, regardless of what the client offers.
 */
static int cap_ssh_user_auth(byte authType, WS_UserAuthData *authData, void *ctx)
{
    cap_ssh_state_t *state = (cap_ssh_state_t *)ctx;

    if (authType != WOLFSSH_USERAUTH_PUBLICKEY) {
        return WOLFSSH_USERAUTH_FAILURE;
    }
    if (!state) {
        return WOLFSSH_USERAUTH_FAILURE;
    }

    xSemaphoreTake(state->lock, portMAX_DELAY);
    bool match = state->authkey_len > 0 &&
                 authData->sf.publicKey.publicKeySz == state->authkey_len &&
                 cap_ssh_constant_time_eq(authData->sf.publicKey.publicKey, state->authkey, state->authkey_len);
    xSemaphoreGive(state->lock);

    if (!match) {
        ESP_LOGW(TAG, "SSH auth rejected for user '%.*s'", (int)authData->usernameSz, authData->username);
        return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
    }
    ESP_LOGI(TAG, "SSH auth accepted for user '%.*s'", (int)authData->usernameSz, authData->username);
    return WOLFSSH_USERAUTH_SUCCESS;
}

static void cap_ssh_send(WOLFSSH *ssh, const char *text)
{
    size_t len = strlen(text);
    if (len > 0) {
        wolfSSH_ChannelIdSend(ssh, 0, (byte *)text, (word32)len);
    }
}

static void cap_ssh_send_crlf(WOLFSSH *ssh, const char *text)
{
    cap_ssh_send(ssh, text);
    cap_ssh_send(ssh, "\r\n");
}

/*
 * A small, purpose-built shell — NOT a proxy for the full esp_console REPL.
 * Redirecting esp_console's global stdout/stdin per SSH session would risk
 * corrupting concurrent serial-console output and is a known footgun on
 * ESP-IDF; instead this calls the same underlying primitives the real
 * console's "cap" command uses (claw_cap_call, claw_cap_build_catalog)
 * directly, writing responses straight to the SSH channel.
 *
 * Returns true if the session loop should close the connection (the user
 * typed exit/quit). Without this, a client that reaches EOF on stdin (e.g.
 * `ssh ... < script`) has no way to signal it's done: the server only finds
 * out several seconds later when the client's own idle logic gives up and
 * sends a disconnect — live-verified as the cause of what looked like a
 * post-auth hang before this was added.
 */
static bool cap_ssh_dispatch_line(WOLFSSH *ssh, const char *line, char *output, size_t output_size)
{
    if (line[0] == '\0') {
        return false;
    }
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        cap_ssh_send_crlf(ssh, "Goodbye.");
        return true;
    }
    if (strcmp(line, "help") == 0) {
        cap_ssh_send_crlf(ssh, "Commands: help, list, groups, call <name> <json>, exit");
        return false;
    }
    if (strcmp(line, "list") == 0) {
        char *catalog = claw_cap_build_catalog();
        if (catalog) {
            cap_ssh_send_crlf(ssh, catalog);
            free(catalog);
        }
        return false;
    }
    if (strcmp(line, "groups") == 0) {
        claw_cap_group_list_t groups = claw_cap_list_groups();
        for (size_t i = 0; i < groups.count; i++) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s [%s]", groups.items[i].group_id,
                     claw_cap_state_to_string(groups.items[i].state));
            cap_ssh_send_crlf(ssh, buf);
        }
        return false;
    }
    if (strncmp(line, "call ", 5) == 0) {
        const char *rest = line + 5;
        const char *sp = strchr(rest, ' ');
        if (!sp) {
            cap_ssh_send_crlf(ssh, "Usage: call <name> <json>");
            return false;
        }
        char name[64];
        size_t name_len = (size_t)(sp - rest);
        if (name_len >= sizeof(name)) {
            name_len = sizeof(name) - 1;
        }
        memcpy(name, rest, name_len);
        name[name_len] = '\0';
        const char *json = sp + 1;

        /* Same trust level as a real serial-console "cap call" — see
         * app_claw_cli.c's cmd_cap_call(), which also uses CONSOLE. An SSH
         * session only reaches here after a successful public-key handshake. */
        claw_cap_call_context_t ctx = { .caller = CLAW_CAP_CALLER_CONSOLE };
        claw_cap_call(name, json, &ctx, output, output_size);
        cap_ssh_send_crlf(ssh, output);
        return false;
    }
    cap_ssh_send_crlf(ssh, "Unknown command. Type 'help'.");
    return false;
}

/* Mirrors the read/dispatch loop shape of wolfSSH's own reference server
 * (examples/echoserver/main/echoserver.c ssh_worker()), replacing its echo
 * behavior with line buffering + cap_ssh_dispatch_line(). Channel id 0 is
 * the session/shell channel wolfSSH_accept() already established. */
static void cap_ssh_session_loop(WOLFSSH *ssh)
{
    WS_SOCKET_T sshFd = wolfSSH_get_fd(ssh);
    char line[CAP_SSH_LINE_MAX];
    size_t line_len = 0;
    char *output = malloc(CAP_SSH_RESP_MAX);
    if (!output) {
        ESP_LOGE(TAG, "session: out of memory");
        return;
    }

    cap_ssh_send_crlf(ssh, "ESP-Claw SSH console. Type 'help'.");

    for (;;) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(sshFd, &readFds);
        if (select((int)sshFd + 1, &readFds, NULL, NULL, NULL) < 0) {
            break;
        }
        if (!FD_ISSET(sshFd, &readFds)) {
            continue;
        }

        word32 lastChannel = 0;
        int ret = wolfSSH_worker(ssh, &lastChannel);
        if (ret >= 0) {
            continue;
        }

        int err = wolfSSH_get_error(ssh);
        if (err == WS_WANT_READ || err == WS_WANT_WRITE || err == WS_CHANNEL_CLOSED) {
            /* WS_CHANNEL_CLOSED is a normal, non-fatal internal-channel-lifecycle
             * signal during setup (matches wolfSSH's own reference server —
             * examples/echoserver/main/echoserver.c ssh_worker() — treating it
             * as fatal here made real sessions die silently right after the
             * banner, before any client data was ever read; live-verified). */
            continue;
        }
        if (err != WS_CHAN_RXD || lastChannel != 0) {
            break; /* real error, EOF, or activity on a channel we don't use */
        }

        byte buf[256];
        int n = wolfSSH_ChannelIdRead(ssh, 0, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        bool should_close = false;
        for (int i = 0; i < n && !should_close; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    should_close = cap_ssh_dispatch_line(ssh, line, output, CAP_SSH_RESP_MAX);
                    line_len = 0;
                }
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
        if (should_close) {
            break;
        }
    }

    free(output);
}

static void cap_ssh_server_task(void *arg)
{
    (void)arg;

    WOLFSSH_CTX *ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "wolfSSH_CTX_new failed");
        s_ssh.server_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    wolfSSH_SetUserAuth(ctx, cap_ssh_user_auth);
    wolfSSH_CTX_SetBanner(ctx, "ESP-Claw SSH console\r\n");

    xSemaphoreTake(s_ssh.lock, portMAX_DELAY);
    int hkRet = wolfSSH_CTX_UsePrivateKey_buffer(ctx, s_ssh.hostkey, (word32)s_ssh.hostkey_len,
                                                 WOLFSSH_FORMAT_ASN1);
    xSemaphoreGive(s_ssh.lock);
    if (hkRet != WS_SUCCESS) {
        ESP_LOGE(TAG, "failed to load SSH host key: %d", hkRet);
        wolfSSH_CTX_free(ctx);
        s_ssh.server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        ESP_LOGE(TAG, "socket() failed");
        wolfSSH_CTX_free(ctx);
        s_ssh.server_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(CAP_SSH_PORT);
    if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listenFd, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen on port %d failed", CAP_SSH_PORT);
        close(listenFd);
        wolfSSH_CTX_free(ctx);
        s_ssh.server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SSH server listening on port %d", CAP_SSH_PORT);

    for (;;) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientFd < 0) {
            continue;
        }
        ESP_LOGI(TAG, "SSH connection accepted");

        WOLFSSH *ssh = wolfSSH_new(ctx);
        if (!ssh) {
            close(clientFd);
            continue;
        }
        wolfSSH_SetUserAuthCtx(ssh, &s_ssh);
        wolfSSH_set_fd(ssh, clientFd);

        int ret = wolfSSH_accept(ssh);
        if (ret == WS_SUCCESS) {
            cap_ssh_session_loop(ssh);
        } else {
            ESP_LOGW(TAG, "wolfSSH_accept failed: %d (%s)", ret, wolfSSH_get_error_name(ssh));
        }

        wolfSSH_shutdown(ssh);
        wolfSSH_free(ssh);
        close(clientFd);
        ESP_LOGI(TAG, "SSH connection closed");
    }
}

static esp_err_t cap_ssh_start_server(void)
{
    if (s_ssh.server_task) {
        return ESP_OK; /* already running; v1 doesn't support live reconfigure-restart */
    }
    BaseType_t ok = xTaskCreate(cap_ssh_server_task, "cap_ssh", CAP_SSH_SERVER_STACK, NULL, 5,
                                &s_ssh.server_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t cap_ssh_group_init(void)
{
    esp_err_t err = cap_ssh_ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    /* Config already applied by app_capabilities' SSH "prepare" hook (which
     * runs before group registration) via cap_ssh_set_config() — that call
     * already started the server if enabled+configured. This is just a
     * defensive no-op re-check for callers that register the group without
     * going through that hook (cap_ssh_start_server() is idempotent). */
    if (s_ssh.enabled && s_ssh.configured) {
        cap_ssh_start_server();
    }
    return ESP_OK;
}

/*
 * ssh_configure: sets both the device's own SSH host identity key (DER,
 * base64-encoded — e.g. `openssl ec -in hostkey.pem -outform DER | base64 -w0`
 * from an operator-generated `ssh-keygen -m PEM -t ecdsa -f hostkey`, same
 * "operator provides the private key" precedent as wireguard_configure's
 * private_key field — DER rather than PEM because wolfSSH's PEM host-key
 * loading returned WS_UNIMPLEMENTED_E in live testing on this build) and the
 * one authorized client public key (standard OpenSSH pubkey line).
 * Root-agent only — not reachable over MQTT, by the same caller-check every
 * other ROOT_AGENT_ONLY tool relies on. Starts the SSH server on first
 * successful configure; v1 does not support a live restart on reconfigure
 * (requires a reboot to pick up a changed key). Applies via cap_ssh_set_config()
 * and persists through the app-provided hook, exactly like vpn_configure —
 * this tool never touches storage directly.
 */
static esp_err_t cap_ssh_configure_execute(const char *input_json,
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

    const char *host_key_b64 = cap_ssh_json_str(input, "host_private_key_der_b64");
    const char *authorized_key_line = cap_ssh_json_str(input, "authorized_public_key");
    if (!host_key_b64 || !host_key_b64[0] || !authorized_key_line || !authorized_key_line[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "Error: both 'host_private_key_der_b64' (base64-encoded DER) and "
                 "'authorized_public_key' (OpenSSH public key line) are required");
        return ESP_ERR_INVALID_ARG;
    }

    char host_key_buf[CAP_SSH_HOSTKEY_B64_MAX];
    char auth_key_buf[CAP_SSH_AUTHKEY_LINE_MAX];
    strlcpy(host_key_buf, host_key_b64, sizeof(host_key_buf));
    strlcpy(auth_key_buf, authorized_key_line, sizeof(auth_key_buf));
    cJSON_Delete(input);

    cap_ssh_config_t cfg = {
        .enabled = true,
        .host_private_key_der_b64 = host_key_buf,
        .authorized_public_key = auth_key_buf,
    };
    bool already_running = s_ssh.server_task != NULL;
    esp_err_t err = cap_ssh_set_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: invalid host_private_key_der_b64 (base64/DER) or authorized_public_key "
                 "(OpenSSH public key line)");
        return err;
    }

    bool persisted = cap_ssh_persist_current();

    snprintf(output, output_size,
             "{\"ok\":true,\"persisted\":%s,\"note\":\"SSH configuration saved.%s\"}",
             persisted ? "true" : "false",
             already_running ? " Server already running (reboot to apply changed keys)."
                              : " Server starting on port 22.");
    return ESP_OK;
}

static esp_err_t cap_ssh_status_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)input_json;
    (void)ctx;

    xSemaphoreTake(s_ssh.lock, portMAX_DELAY);
    bool configured = s_ssh.configured;
    bool enabled = s_ssh.enabled;
    xSemaphoreGive(s_ssh.lock);

    snprintf(output, output_size,
             "{\"enabled\":%s,\"configured\":%s,\"running\":%s,\"port\":%d}",
             enabled ? "true" : "false",
             configured ? "true" : "false",
             s_ssh.server_task ? "true" : "false",
             CAP_SSH_PORT);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_ssh_descriptors[] = {
    {
        .id = "ssh_configure",
        .name = "ssh_configure",
        .family = "system",
        .description = "Set the device's SSH host identity key (base64-encoded DER) and the one "
                       "authorized client public key (OpenSSH format, RSA or ECDSA only — "
                       "ed25519 keys are rejected by this build). Root-agent only — not "
                       "reachable over MQTT. Starts the SSH console server (public-key auth only, "
                       "port 22) once both are set.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"host_private_key_der_b64\":{\"type\":\"string\","
        "\"description\":\"base64-encoded DER SSH host private key\"},"
        "\"authorized_public_key\":{\"type\":\"string\",\"description\":\"OpenSSH public key line "
        "(RSA or ECDSA only; ed25519 is rejected by this build)\"}},"
        "\"required\":[\"host_private_key_der_b64\",\"authorized_public_key\"]}",
        .execute = cap_ssh_configure_execute,
    },
    {
        .id = "ssh_status",
        .name = "ssh_status",
        .family = "system",
        .description = "Report whether the SSH console is configured and running, and its port. "
                       "Never returns keys.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_ssh_status_execute,
    },
};

static const claw_cap_group_t s_ssh_group = {
    .group_id = "cap_ssh",
    .descriptors = s_ssh_descriptors,
    .descriptor_count = sizeof(s_ssh_descriptors) / sizeof(s_ssh_descriptors[0]),
    .group_init = cap_ssh_group_init,
};

esp_err_t cap_ssh_register_group(void)
{
    if (claw_cap_group_exists(s_ssh_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_ssh_group);
}
