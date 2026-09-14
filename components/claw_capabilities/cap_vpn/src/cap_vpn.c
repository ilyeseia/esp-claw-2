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

static const char *TAG = "cap_vpn";

#define CAP_VPN_HOST_MAX            128
#define CAP_VPN_CONNECT_TIMEOUT_MS  5000

typedef struct {
    bool     enabled;
    char     gateway[CAP_VPN_HOST_MAX];
    char     test_host[CAP_VPN_HOST_MAX];
    uint16_t test_port;
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

/* Non-blocking TCP connect bounded by a timeout. Returns true on success and
 * writes the elapsed milliseconds to out_ms. */
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

    uint16_t port = s_vpn.test_port ? s_vpn.test_port : 80;
    cJSON_AddStringToObject(root, "mode", "tailscale-gateway");
    cJSON_AddBoolToObject(root, "enabled", s_vpn.enabled);
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

/*
 * vpn_configure: set the Tailscale gateway settings from a chat/tool call, apply
 * live and (if a persist provider is wired) save to NVS. Root-agent only +
 * restricted. Omitted fields keep their current value. No secrets are involved.
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

    bool enabled = cap_vpn_json_bool(input, "enabled", true);
    int port = cap_vpn_json_int(input, "test_port", s_vpn.test_port ? s_vpn.test_port : 80);
    const char *gateway = cap_vpn_json_str(input, "gateway");
    const char *test_host = cap_vpn_json_str(input, "test_host");

    /* Preserve existing values when a field is omitted. */
    char gw_buf[CAP_VPN_HOST_MAX];
    char host_buf[CAP_VPN_HOST_MAX];
    strlcpy(gw_buf, gateway ? gateway : s_vpn.gateway, sizeof(gw_buf));
    strlcpy(host_buf, test_host ? test_host : s_vpn.test_host, sizeof(host_buf));
    cJSON_Delete(input);

    if (port < 1 || port > 65535) {
        snprintf(output, output_size, "Error: test_port must be between 1 and 65535");
        return ESP_ERR_INVALID_ARG;
    }

    cap_vpn_config_t cfg = {
        .enabled = enabled,
        .gateway = gw_buf,
        .test_host = host_buf,
        .test_port = (uint16_t)port,
    };

    esp_err_t err = cap_vpn_set_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to apply VPN config (%s)", esp_err_to_name(err));
        return err;
    }

    bool persisted = false;
    if (s_persist) {
        esp_err_t perr = s_persist(&cfg, s_persist_ctx);
        persisted = (perr == ESP_OK);
        if (!persisted) {
            ESP_LOGW(TAG, "VPN config applied live but persist failed: %s", esp_err_to_name(perr));
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "gateway", gw_buf);
    cJSON_AddStringToObject(root, "test_host", host_buf);
    cJSON_AddNumberToObject(root, "test_port", port);
    cJSON_AddBoolToObject(root, "persisted", persisted);
    cJSON_AddStringToObject(root, "note",
                            "Saved. Run vpn_status to probe the route. Reachability depends on the "
                            "Tailscale subnet-router gateway on your LAN, not on the device itself.");
    return cap_vpn_emit(root, output, output_size);
}

static const claw_cap_descriptor_t s_vpn_descriptors[] = {
    {
        .id = "vpn_status",
        .name = "vpn_status",
        .family = "system",
        .description = "Report the Tailscale gateway VPN configuration and probe reachability of the "
                       "configured tailnet host (DNS resolution + TCP connect through the gateway).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_vpn_status_execute,
    },
    {
        .id = "vpn_configure",
        .name = "vpn_configure",
        .family = "network",
        .description = "Configure the Tailscale gateway VPN settings (enabled/gateway/test_host/test_port), "
                       "apply live and persist across reboots. Restricted, root-agent only. The device does "
                       "not itself join the tailnet — a LAN subnet-router gateway bridges it.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_RESTRICTED |
                     CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"gateway\":{\"type\":\"string\",\"description\":\"subnet-router LAN host/IP\"},"
        "\"test_host\":{\"type\":\"string\",\"description\":\"tailnet host to probe\"},"
        "\"test_port\":{\"type\":\"integer\"}}}",
        .execute = cap_vpn_configure_execute,
    },
};

static const claw_cap_group_t s_vpn_group = {
    .group_id = "cap_vpn",
    .descriptors = s_vpn_descriptors,
    .descriptor_count = sizeof(s_vpn_descriptors) / sizeof(s_vpn_descriptors[0]),
};

esp_err_t cap_vpn_set_config(const cap_vpn_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_vpn.enabled = config->enabled;
    strlcpy(s_vpn.gateway, config->gateway ? config->gateway : "", sizeof(s_vpn.gateway));
    strlcpy(s_vpn.test_host, config->test_host ? config->test_host : "", sizeof(s_vpn.test_host));
    s_vpn.test_port = config->test_port ? config->test_port : 80;

    ESP_LOGI(TAG, "VPN config: enabled=%d gateway=%s test_host=%s:%u",
             (int)s_vpn.enabled, s_vpn.gateway, s_vpn.test_host, (unsigned)s_vpn.test_port);
    return ESP_OK;
}

esp_err_t cap_vpn_register_group(void)
{
    if (claw_cap_group_exists(s_vpn_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_vpn_group);
}
