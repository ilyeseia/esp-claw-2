/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_vpn.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "esp_timer.h"

#if CONFIG_APP_CLAW_VPN_WIREGUARD
#include "wireguard_manager.h"
#endif

static const char *TAG = "cap_vpn";

#define CAP_VPN_HOST_MAX            128
#define CAP_VPN_KEY_MAX             64
#define CAP_VPN_ADDR_MAX            48
#define CAP_VPN_CONNECT_TIMEOUT_MS  5000

#define CAP_VPN_MODE_OFF        "off"
#define CAP_VPN_MODE_GATEWAY    "tailscale-gateway"
#define CAP_VPN_MODE_WIREGUARD  "wireguard"

typedef struct {
    char     mode[24];
    bool     enabled;
    char     gateway[CAP_VPN_HOST_MAX];
    char     test_host[CAP_VPN_HOST_MAX];
    uint16_t test_port;

    /* WireGuard parameters (retained for status / reconnect / persist). */
    char     wg_private_key[CAP_VPN_KEY_MAX];
    char     wg_address[CAP_VPN_ADDR_MAX];
    char     wg_peer_public_key[CAP_VPN_KEY_MAX];
    char     wg_endpoint[CAP_VPN_HOST_MAX];
    uint16_t wg_endpoint_port;
    char     wg_allowed_ips[CAP_VPN_HOST_MAX];
    uint16_t wg_keepalive;
    char     wg_preshared_key[CAP_VPN_KEY_MAX];
    bool     wg_make_default;
#if CONFIG_APP_CLAW_VPN_WIREGUARD
    wireguard_manager_handle_t wg;
#endif
} cap_vpn_state_t;

static cap_vpn_state_t s_vpn;

static cap_vpn_persist_fn s_persist;
static void *s_persist_ctx;

esp_err_t cap_vpn_set_persist_provider(cap_vpn_persist_fn persist, void *user_ctx)
{
    s_persist = persist;
    s_persist_ctx = user_ctx;
    return ESP_OK;
}

static bool cap_vpn_mode_is(const char *mode)
{
    return strcmp(s_vpn.mode, mode) == 0;
}

/* Emit a cJSON object into the caller-provided output buffer, then free it. */
static esp_err_t cap_vpn_emit(cJSON *root, char *output, size_t output_size)
{
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(output, output_size, "Error: failed to encode result");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

static const char *cap_vpn_json_str(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

static bool cap_vpn_json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
    }
    return fallback;
}

static int cap_vpn_json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return atoi(item->valuestring);
    }
    return fallback;
}

/* Non-blocking TCP connect bounded by a timeout (used by the gateway probe). */
static bool cap_vpn_try_connect(const struct addrinfo *ai, int timeout_ms, int *out_ms)
{
    int64_t t0 = esp_timer_get_time();
    bool ok = false;

    int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) {
        if (out_ms) {
            *out_ms = 0;
        }
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        if (select(sock + 1, NULL, &wset, NULL, &tv) > 0) {
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) {
                ok = true;
            }
        }
    }

    close(sock);
    if (out_ms) {
        *out_ms = (int)((esp_timer_get_time() - t0) / 1000);
    }
    return ok;
}

#if CONFIG_APP_CLAW_VPN_WIREGUARD
/* (Re)build the WireGuard tunnel from the retained parameters. */
static esp_err_t cap_vpn_wg_apply(bool connect_now)
{
    if (s_vpn.wg) {
        wireguard_manager_destroy(s_vpn.wg);
        s_vpn.wg = NULL;
    }
    if (!s_vpn.wg_private_key[0] || !s_vpn.wg_peer_public_key[0] || !s_vpn.wg_endpoint[0]) {
        ESP_LOGW(TAG, "WireGuard mode selected but private_key/peer_public_key/endpoint incomplete");
        return ESP_ERR_INVALID_STATE;
    }

    wireguard_manager_config_t cfg = {
        .private_key = s_vpn.wg_private_key,
        .address = s_vpn.wg_address,
        .peer_public_key = s_vpn.wg_peer_public_key,
        .endpoint = s_vpn.wg_endpoint,
        .endpoint_port = s_vpn.wg_endpoint_port,
        .preshared_key = s_vpn.wg_preshared_key[0] ? s_vpn.wg_preshared_key : NULL,
        .keepalive = s_vpn.wg_keepalive,
        .make_default = s_vpn.wg_make_default,
    };

    esp_err_t err = wireguard_manager_create(&cfg, &s_vpn.wg);
    if (err != ESP_OK) {
        s_vpn.wg = NULL;
        return err;
    }
    if (connect_now) {
        err = wireguard_manager_connect(s_vpn.wg);
    }
    return err;
}
#endif /* CONFIG_APP_CLAW_VPN_WIREGUARD */

esp_err_t cap_vpn_set_config(const cap_vpn_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_vpn.mode,
            (config->mode && config->mode[0]) ? config->mode : CAP_VPN_MODE_GATEWAY,
            sizeof(s_vpn.mode));
    s_vpn.enabled = config->enabled;
    strlcpy(s_vpn.gateway, config->gateway ? config->gateway : "", sizeof(s_vpn.gateway));
    strlcpy(s_vpn.test_host, config->test_host ? config->test_host : "", sizeof(s_vpn.test_host));
    s_vpn.test_port = config->test_port ? config->test_port : 80;

    strlcpy(s_vpn.wg_private_key, config->wg_private_key ? config->wg_private_key : "",
            sizeof(s_vpn.wg_private_key));
    strlcpy(s_vpn.wg_address, config->wg_address ? config->wg_address : "", sizeof(s_vpn.wg_address));
    strlcpy(s_vpn.wg_peer_public_key, config->wg_peer_public_key ? config->wg_peer_public_key : "",
            sizeof(s_vpn.wg_peer_public_key));
    strlcpy(s_vpn.wg_endpoint, config->wg_endpoint ? config->wg_endpoint : "", sizeof(s_vpn.wg_endpoint));
    s_vpn.wg_endpoint_port = config->wg_endpoint_port ? config->wg_endpoint_port : 51820;
    strlcpy(s_vpn.wg_allowed_ips, config->wg_allowed_ips ? config->wg_allowed_ips : "",
            sizeof(s_vpn.wg_allowed_ips));
    s_vpn.wg_keepalive = config->wg_keepalive;
    strlcpy(s_vpn.wg_preshared_key, config->wg_preshared_key ? config->wg_preshared_key : "",
            sizeof(s_vpn.wg_preshared_key));
    s_vpn.wg_make_default = config->wg_make_default;

    ESP_LOGI(TAG, "VPN config: mode=%s enabled=%d gateway=%s test=%s:%u wg_endpoint=%s:%u",
             s_vpn.mode, (int)s_vpn.enabled, s_vpn.gateway, s_vpn.test_host,
             (unsigned)s_vpn.test_port, s_vpn.wg_endpoint, (unsigned)s_vpn.wg_endpoint_port);

#if CONFIG_APP_CLAW_VPN_WIREGUARD
    if (cap_vpn_mode_is(CAP_VPN_MODE_WIREGUARD)) {
        esp_err_t werr = cap_vpn_wg_apply(s_vpn.enabled);
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "WireGuard apply: %s", esp_err_to_name(werr));
        }
    } else if (s_vpn.wg) {
        wireguard_manager_destroy(s_vpn.wg);
        s_vpn.wg = NULL;
    }
#endif
    return ESP_OK;
}

/* Gateway-mode reachability probe (DNS + TCP connect through the LAN gateway). */
static esp_err_t cap_vpn_status_gateway(cJSON *root, char *output, size_t output_size)
{
    uint16_t port = s_vpn.test_port ? s_vpn.test_port : 80;
    cJSON_AddStringToObject(root, "gateway", s_vpn.gateway);
    cJSON_AddStringToObject(root, "test_host", s_vpn.test_host);
    cJSON_AddNumberToObject(root, "test_port", port);

    if (!s_vpn.enabled) {
        cJSON_AddStringToObject(root, "note", "VPN gateway integration disabled");
        return cap_vpn_emit(root, output, output_size);
    }
    if (!s_vpn.test_host[0]) {
        cJSON_AddStringToObject(root, "note", "No tailnet test host configured");
        return cap_vpn_emit(root, output, output_size);
    }

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;

    int64_t t0 = esp_timer_get_time();
    int gai = getaddrinfo(s_vpn.test_host, portstr, &hints, &res);
    int dns_ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (gai != 0 || !res) {
        cJSON_AddBoolToObject(root, "dns_ok", false);
        cJSON_AddBoolToObject(root, "reachable", false);
        cJSON_AddStringToObject(root, "error",
                                "DNS resolution failed — is the tailnet route/MagicDNS reachable via the gateway?");
        if (res) {
            freeaddrinfo(res);
        }
        return cap_vpn_emit(root, output, output_size);
    }

    cJSON_AddBoolToObject(root, "dns_ok", true);
    cJSON_AddNumberToObject(root, "dns_ms", dns_ms);

    char ipstr[INET6_ADDRSTRLEN] = {0};
    if (res->ai_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
    } else if (res->ai_family == AF_INET6) {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)res->ai_addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ipstr, sizeof(ipstr));
    }
    cJSON_AddStringToObject(root, "resolved_ip", ipstr);

    int connect_ms = 0;
    bool reachable = cap_vpn_try_connect(res, CAP_VPN_CONNECT_TIMEOUT_MS, &connect_ms);
    freeaddrinfo(res);

    cJSON_AddBoolToObject(root, "reachable", reachable);
    cJSON_AddNumberToObject(root, "connect_ms", connect_ms);
    if (!reachable) {
        cJSON_AddStringToObject(root, "error",
                                "TCP connect failed — gateway/route down or the tailnet host is offline");
    }
    return cap_vpn_emit(root, output, output_size);
}

static esp_err_t cap_vpn_status_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "mode", s_vpn.mode[0] ? s_vpn.mode : CAP_VPN_MODE_GATEWAY);

    if (cap_vpn_mode_is(CAP_VPN_MODE_WIREGUARD)) {
#if CONFIG_APP_CLAW_VPN_WIREGUARD
        cJSON_AddBoolToObject(root, "enabled", s_vpn.enabled);
        cJSON_AddStringToObject(root, "address", s_vpn.wg_address);
        cJSON_AddStringToObject(root, "endpoint", s_vpn.wg_endpoint);
        cJSON_AddNumberToObject(root, "endpoint_port", s_vpn.wg_endpoint_port);
        cJSON_AddBoolToObject(root, "peer_key_set", s_vpn.wg_peer_public_key[0] != '\0');
        cJSON_AddBoolToObject(root, "full_tunnel", s_vpn.wg_make_default);
        if (s_vpn.wg) {
            wireguard_manager_status_t st = {0};
            wireguard_manager_get_status(s_vpn.wg, &st);
            cJSON_AddBoolToObject(root, "tunnel_up", st.connected);
            cJSON_AddStringToObject(root, "note",
                                    st.connected ? "WireGuard handshake up."
                                                 : "WireGuard configured; waiting for handshake "
                                                   "(check endpoint/keys and that system time is set).");
        } else {
            cJSON_AddBoolToObject(root, "tunnel_up", false);
            cJSON_AddStringToObject(root, "note",
                                    "WireGuard not started — set keys/endpoint and enable, then vpn_connect.");
        }
        return cap_vpn_emit(root, output, output_size);
#else
        cJSON_AddStringToObject(root, "note",
                                "WireGuard support not built (enable APP_CLAW_VPN_WIREGUARD).");
        return cap_vpn_emit(root, output, output_size);
#endif
    }

    if (cap_vpn_mode_is(CAP_VPN_MODE_OFF)) {
        cJSON_AddStringToObject(root, "note", "VPN disabled");
        return cap_vpn_emit(root, output, output_size);
    }

    /* Default: Tailscale gateway mode. */
    cJSON_AddBoolToObject(root, "enabled", s_vpn.enabled);
    return cap_vpn_status_gateway(root, output, output_size);
}

/* Persist current state through the app-provided hook, if any. */
static bool cap_vpn_persist_current(void)
{
    if (!s_persist) {
        return false;
    }
    cap_vpn_config_t cfg = {
        .mode = s_vpn.mode,
        .enabled = s_vpn.enabled,
        .gateway = s_vpn.gateway,
        .test_host = s_vpn.test_host,
        .test_port = s_vpn.test_port,
        .wg_private_key = s_vpn.wg_private_key,
        .wg_address = s_vpn.wg_address,
        .wg_peer_public_key = s_vpn.wg_peer_public_key,
        .wg_endpoint = s_vpn.wg_endpoint,
        .wg_endpoint_port = s_vpn.wg_endpoint_port,
        .wg_allowed_ips = s_vpn.wg_allowed_ips,
        .wg_keepalive = s_vpn.wg_keepalive,
        .wg_preshared_key = s_vpn.wg_preshared_key,
        .wg_make_default = s_vpn.wg_make_default,
    };
    esp_err_t err = s_persist(&cfg, s_persist_ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "VPN persist failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/*
 * vpn_configure: set mode + Tailscale gateway settings. Root-agent only +
 * restricted. Omitted fields keep their current value. No secrets here.
 */
static esp_err_t cap_vpn_configure_execute(const char *input_json,
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

    const char *mode = cap_vpn_json_str(input, "mode");
    bool enabled = cap_vpn_json_bool(input, "enabled", s_vpn.enabled);
    int port = cap_vpn_json_int(input, "test_port", s_vpn.test_port ? s_vpn.test_port : 80);
    const char *gateway = cap_vpn_json_str(input, "gateway");
    const char *test_host = cap_vpn_json_str(input, "test_host");

    char mode_buf[24];
    char gw_buf[CAP_VPN_HOST_MAX];
    char host_buf[CAP_VPN_HOST_MAX];
    strlcpy(mode_buf, mode ? mode : (s_vpn.mode[0] ? s_vpn.mode : CAP_VPN_MODE_GATEWAY), sizeof(mode_buf));
    strlcpy(gw_buf, gateway ? gateway : s_vpn.gateway, sizeof(gw_buf));
    strlcpy(host_buf, test_host ? test_host : s_vpn.test_host, sizeof(host_buf));
    cJSON_Delete(input);

    if (strcmp(mode_buf, CAP_VPN_MODE_OFF) != 0 &&
            strcmp(mode_buf, CAP_VPN_MODE_GATEWAY) != 0 &&
            strcmp(mode_buf, CAP_VPN_MODE_WIREGUARD) != 0) {
        snprintf(output, output_size, "Error: mode must be off, tailscale-gateway or wireguard");
        return ESP_ERR_INVALID_ARG;
    }
    if (port < 1 || port > 65535) {
        snprintf(output, output_size, "Error: test_port must be between 1 and 65535");
        return ESP_ERR_INVALID_ARG;
    }

    /* Start from current retained values so WireGuard params survive a gateway edit. */
    cap_vpn_config_t cfg = {0};
    cfg.mode = mode_buf;
    cfg.enabled = enabled;
    cfg.gateway = gw_buf;
    cfg.test_host = host_buf;
    cfg.test_port = (uint16_t)port;
    cfg.wg_private_key = s_vpn.wg_private_key;
    cfg.wg_address = s_vpn.wg_address;
    cfg.wg_peer_public_key = s_vpn.wg_peer_public_key;
    cfg.wg_endpoint = s_vpn.wg_endpoint;
    cfg.wg_endpoint_port = s_vpn.wg_endpoint_port;
    cfg.wg_allowed_ips = s_vpn.wg_allowed_ips;
    cfg.wg_keepalive = s_vpn.wg_keepalive;
    cfg.wg_preshared_key = s_vpn.wg_preshared_key;
    cfg.wg_make_default = s_vpn.wg_make_default;

    esp_err_t err = cap_vpn_set_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to apply VPN config (%s)", esp_err_to_name(err));
        return err;
    }
    bool persisted = cap_vpn_persist_current();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "mode", mode_buf);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "gateway", gw_buf);
    cJSON_AddStringToObject(root, "test_host", host_buf);
    cJSON_AddNumberToObject(root, "test_port", port);
    cJSON_AddBoolToObject(root, "persisted", persisted);
    cJSON_AddStringToObject(root, "note", "Saved. Run vpn_status to check the selected mode.");
    return cap_vpn_emit(root, output, output_size);
}

#if CONFIG_APP_CLAW_VPN_WIREGUARD
/*
 * wireguard_configure: set the on-device WireGuard tunnel parameters, switch to
 * wireguard mode, apply live and persist. Root-agent only + restricted. Secrets
 * (private/preshared keys) are accepted but never echoed back.
 */
static esp_err_t cap_vpn_wireguard_configure_execute(const char *input_json,
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

    const char *private_key = cap_vpn_json_str(input, "private_key");
    const char *address = cap_vpn_json_str(input, "address");
    const char *peer_public_key = cap_vpn_json_str(input, "peer_public_key");
    const char *endpoint = cap_vpn_json_str(input, "endpoint");
    const char *allowed_ips = cap_vpn_json_str(input, "allowed_ips");
    const char *preshared_key = cap_vpn_json_str(input, "preshared_key");
    int port = cap_vpn_json_int(input, "endpoint_port", s_vpn.wg_endpoint_port ? s_vpn.wg_endpoint_port : 51820);
    int keepalive = cap_vpn_json_int(input, "keepalive", s_vpn.wg_keepalive);
    bool make_default = cap_vpn_json_bool(input, "make_default", s_vpn.wg_make_default);
    bool enabled = cap_vpn_json_bool(input, "enabled", true);

    /* Preserve existing values when a field is omitted. */
    char priv_buf[CAP_VPN_KEY_MAX];
    char addr_buf[CAP_VPN_ADDR_MAX];
    char peer_buf[CAP_VPN_KEY_MAX];
    char ep_buf[CAP_VPN_HOST_MAX];
    char allowed_buf[CAP_VPN_HOST_MAX];
    char psk_buf[CAP_VPN_KEY_MAX];
    strlcpy(priv_buf, private_key ? private_key : s_vpn.wg_private_key, sizeof(priv_buf));
    strlcpy(addr_buf, address ? address : s_vpn.wg_address, sizeof(addr_buf));
    strlcpy(peer_buf, peer_public_key ? peer_public_key : s_vpn.wg_peer_public_key, sizeof(peer_buf));
    strlcpy(ep_buf, endpoint ? endpoint : s_vpn.wg_endpoint, sizeof(ep_buf));
    strlcpy(allowed_buf, allowed_ips ? allowed_ips : s_vpn.wg_allowed_ips, sizeof(allowed_buf));
    strlcpy(psk_buf, preshared_key ? preshared_key : s_vpn.wg_preshared_key, sizeof(psk_buf));
    cJSON_Delete(input);

    if (!priv_buf[0] || !peer_buf[0] || !ep_buf[0]) {
        snprintf(output, output_size,
                 "Error: private_key, peer_public_key and endpoint are required");
        return ESP_ERR_INVALID_ARG;
    }
    if (port < 1 || port > 65535) {
        snprintf(output, output_size, "Error: endpoint_port must be between 1 and 65535");
        return ESP_ERR_INVALID_ARG;
    }
    if (keepalive < 0 || keepalive > 65535) {
        keepalive = 0;
    }

    cap_vpn_config_t cfg = {
        .mode = CAP_VPN_MODE_WIREGUARD,
        .enabled = enabled,
        .gateway = s_vpn.gateway,
        .test_host = s_vpn.test_host,
        .test_port = s_vpn.test_port,
        .wg_private_key = priv_buf,
        .wg_address = addr_buf,
        .wg_peer_public_key = peer_buf,
        .wg_endpoint = ep_buf,
        .wg_endpoint_port = (uint16_t)port,
        .wg_allowed_ips = allowed_buf,
        .wg_keepalive = (uint16_t)keepalive,
        .wg_preshared_key = psk_buf,
        .wg_make_default = make_default,
    };

    esp_err_t err = cap_vpn_set_config(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        snprintf(output, output_size, "Error: failed to apply WireGuard config (%s)", esp_err_to_name(err));
        return err;
    }
    bool persisted = cap_vpn_persist_current();

    wireguard_manager_status_t st = {0};
    if (s_vpn.wg) {
        wireguard_manager_get_status(s_vpn.wg, &st);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "mode", CAP_VPN_MODE_WIREGUARD);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "address", addr_buf);
    cJSON_AddStringToObject(root, "endpoint", ep_buf);
    cJSON_AddNumberToObject(root, "endpoint_port", port);
    cJSON_AddBoolToObject(root, "full_tunnel", make_default);
    cJSON_AddBoolToObject(root, "tunnel_up", st.connected);
    cJSON_AddBoolToObject(root, "persisted", persisted);
    cJSON_AddStringToObject(root, "note",
                            "Applied live. Handshake needs valid system time; run vpn_status to confirm. "
                            "Keys are stored on the device and never echoed.");
    return cap_vpn_emit(root, output, output_size);
}

static esp_err_t cap_vpn_connect_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    (void)input_json;
    (void)ctx;

    if (!cap_vpn_mode_is(CAP_VPN_MODE_WIREGUARD)) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"not in wireguard mode\"}");
        return ESP_ERR_INVALID_STATE;
    }
    s_vpn.enabled = true;
    esp_err_t err = cap_vpn_wg_apply(true);
    cap_vpn_persist_current();
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"connect failed (%s)\"}",
                 esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size,
             "{\"ok\":true,\"note\":\"WireGuard connect initiated; run vpn_status for handshake state\"}");
    return ESP_OK;
}

static esp_err_t cap_vpn_disconnect_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)input_json;
    (void)ctx;

    s_vpn.enabled = false;
    if (s_vpn.wg) {
        wireguard_manager_disconnect(s_vpn.wg);
    }
    cap_vpn_persist_current();
    snprintf(output, output_size, "{\"ok\":true,\"note\":\"WireGuard tunnel disconnected\"}");
    return ESP_OK;
}
#endif /* CONFIG_APP_CLAW_VPN_WIREGUARD */

static const claw_cap_descriptor_t s_vpn_descriptors[] = {
    {
        .id = "vpn_status",
        .name = "vpn_status",
        .family = "system",
        .description = "Report the VPN mode and status: for tailscale-gateway, probe the tailnet host "
                       "(DNS+TCP); for wireguard, report the tunnel handshake state. Never returns keys.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_vpn_status_execute,
    },
    {
        .id = "vpn_configure",
        .name = "vpn_configure",
        .family = "network",
        .description = "Set VPN mode (off/tailscale-gateway/wireguard) and the Tailscale gateway settings "
                       "(enabled/gateway/test_host/test_port). Applies live and persists. Root-agent only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"off\",\"tailscale-gateway\",\"wireguard\"]},"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"gateway\":{\"type\":\"string\",\"description\":\"subnet-router LAN host/IP\"},"
        "\"test_host\":{\"type\":\"string\",\"description\":\"tailnet host to probe\"},"
        "\"test_port\":{\"type\":\"integer\"}}}",
        .execute = cap_vpn_configure_execute,
    },
#if CONFIG_APP_CLAW_VPN_WIREGUARD
    {
        .id = "wireguard_configure",
        .name = "wireguard_configure",
        .family = "network",
        .description = "Configure the on-device WireGuard tunnel (private_key/address/peer_public_key/"
                       "endpoint/endpoint_port/allowed_ips/keepalive/preshared_key/make_default), switch to "
                       "wireguard mode, apply live and persist. Root-agent only. Keys are never echoed.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"private_key\":{\"type\":\"string\"},"
        "\"address\":{\"type\":\"string\",\"description\":\"device tunnel IP, e.g. 10.2.0.2/32\"},"
        "\"peer_public_key\":{\"type\":\"string\"},"
        "\"endpoint\":{\"type\":\"string\",\"description\":\"peer host or IP\"},"
        "\"endpoint_port\":{\"type\":\"integer\"},"
        "\"allowed_ips\":{\"type\":\"string\"},"
        "\"keepalive\":{\"type\":\"integer\"},"
        "\"preshared_key\":{\"type\":\"string\"},"
        "\"make_default\":{\"type\":\"boolean\",\"description\":\"route all traffic through the tunnel\"},"
        "\"enabled\":{\"type\":\"boolean\"}},"
        "\"required\":[\"private_key\",\"peer_public_key\",\"endpoint\"]}",
        .execute = cap_vpn_wireguard_configure_execute,
    },
    {
        .id = "vpn_connect",
        .name = "vpn_connect",
        .family = "network",
        .description = "Bring the configured WireGuard tunnel up. Root-agent only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_vpn_connect_execute,
    },
    {
        .id = "vpn_disconnect",
        .name = "vpn_disconnect",
        .family = "network",
        .description = "Bring the WireGuard tunnel down. Root-agent only.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_vpn_disconnect_execute,
    },
#endif /* CONFIG_APP_CLAW_VPN_WIREGUARD */
};

static const claw_cap_group_t s_vpn_group = {
    .group_id = "cap_vpn",
    .descriptors = s_vpn_descriptors,
    .descriptor_count = sizeof(s_vpn_descriptors) / sizeof(s_vpn_descriptors[0]),
};

esp_err_t cap_vpn_register_group(void)
{
    if (claw_cap_group_exists(s_vpn_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_vpn_group);
}
