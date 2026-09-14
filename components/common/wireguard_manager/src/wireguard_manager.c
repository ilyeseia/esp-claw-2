/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wireguard_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wireguard.h"

static const char *TAG = "wg_manager";

#define WG_KEY_MAX      64    /* base64 WireGuard keys are 44 chars */
#define WG_ADDR_MAX     48
#define WG_MASK_MAX     24
#define WG_ENDPOINT_MAX 128

typedef struct wireguard_manager_ctx {
    wireguard_config_t wg_config;
    wireguard_ctx_t    wg_ctx;

    /* Owned string storage — esp_wireguard keeps pointers into wg_config. */
    char private_key[WG_KEY_MAX];
    char public_key[WG_KEY_MAX];
    char preshared_key[WG_KEY_MAX];
    char address[WG_ADDR_MAX];
    char mask[WG_MASK_MAX];
    char endpoint[WG_ENDPOINT_MAX];

    uint16_t endpoint_port;
    uint16_t keepalive;
    bool     make_default;
    bool     created;
    bool     connected;
} wireguard_manager_ctx;

/* Convert a CIDR prefix length (0..32) to a dotted-decimal netmask. */
static void wg_cidr_to_mask(int prefix, char *out, size_t out_size)
{
    if (prefix < 0) {
        prefix = 32;
    }
    if (prefix > 32) {
        prefix = 32;
    }
    uint32_t m = prefix ? (0xFFFFFFFFu << (32 - prefix)) : 0u;
    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned)((m >> 24) & 0xFF), (unsigned)((m >> 16) & 0xFF),
             (unsigned)((m >> 8) & 0xFF), (unsigned)(m & 0xFF));
}

/* Split "ip/cidr" into address + dotted mask. Defaults to /32 when no CIDR. */
static void wg_parse_address(const char *address, char *ip_out, size_t ip_size,
                             char *mask_out, size_t mask_size)
{
    const char *slash = address ? strchr(address, '/') : NULL;

    if (!address || !address[0]) {
        ip_out[0] = '\0';
        strlcpy(mask_out, "255.255.255.255", mask_size);
        return;
    }
    if (slash) {
        size_t iplen = (size_t)(slash - address);
        if (iplen >= ip_size) {
            iplen = ip_size - 1;
        }
        memcpy(ip_out, address, iplen);
        ip_out[iplen] = '\0';
        wg_cidr_to_mask(atoi(slash + 1), mask_out, mask_size);
    } else {
        strlcpy(ip_out, address, ip_size);
        strlcpy(mask_out, "255.255.255.255", mask_size);
    }
}

esp_err_t wireguard_manager_create(const wireguard_manager_config_t *config,
                                   wireguard_manager_handle_t *ret_handle)
{
    wireguard_manager_ctx *ctx = NULL;
    esp_err_t err;

    if (!config || !ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->private_key || !config->private_key[0] ||
            !config->peer_public_key || !config->peer_public_key[0] ||
            !config->endpoint || !config->endpoint[0]) {
        ESP_LOGE(TAG, "private_key, peer_public_key and endpoint are required");
        return ESP_ERR_INVALID_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(ctx->private_key, config->private_key, sizeof(ctx->private_key));
    strlcpy(ctx->public_key, config->peer_public_key, sizeof(ctx->public_key));
    if (config->preshared_key && config->preshared_key[0]) {
        strlcpy(ctx->preshared_key, config->preshared_key, sizeof(ctx->preshared_key));
    }
    strlcpy(ctx->endpoint, config->endpoint, sizeof(ctx->endpoint));
    wg_parse_address(config->address, ctx->address, sizeof(ctx->address),
                     ctx->mask, sizeof(ctx->mask));
    ctx->endpoint_port = config->endpoint_port ? config->endpoint_port : 51820;
    ctx->keepalive = config->keepalive;
    ctx->make_default = config->make_default;

    wireguard_config_t wg = ESP_WIREGUARD_CONFIG_DEFAULT();
    wg.private_key = ctx->private_key;
    wg.public_key = ctx->public_key;
    wg.preshared_key = ctx->preshared_key[0] ? ctx->preshared_key : NULL;
    wg.allowed_ip = ctx->address;          /* device's own tunnel IP (esp_wireguard naming) */
    wg.allowed_ip_mask = ctx->mask;
    wg.endpoint = ctx->endpoint;
    wg.port = ctx->endpoint_port;
    wg.persistent_keepalive = ctx->keepalive;
    ctx->wg_config = wg;

    err = esp_wireguard_init(&ctx->wg_config, &ctx->wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_init failed: %s", esp_err_to_name(err));
        free(ctx);
        return err;
    }

    ctx->created = true;
    *ret_handle = ctx;
    ESP_LOGI(TAG, "WireGuard initialized: address=%s/%s endpoint=%s:%u keepalive=%u default=%d",
             ctx->address, ctx->mask, ctx->endpoint, (unsigned)ctx->endpoint_port,
             (unsigned)ctx->keepalive, (int)ctx->make_default);
    return ESP_OK;
}

esp_err_t wireguard_manager_connect(wireguard_manager_handle_t handle)
{
    esp_err_t err;

    if (!handle || !handle->created) {
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_wireguard_connect(&handle->wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_connect failed: %s", esp_err_to_name(err));
        return err;
    }
    if (handle->make_default) {
        err = esp_wireguard_set_default(&handle->wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wireguard_set_default failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "WireGuard connect initiated to %s:%u", handle->endpoint,
             (unsigned)handle->endpoint_port);
    return ESP_OK;
}

esp_err_t wireguard_manager_disconnect(wireguard_manager_handle_t handle)
{
    if (!handle || !handle->created) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_wireguard_disconnect(&handle->wg_ctx);
    handle->connected = false;
    return err;
}

esp_err_t wireguard_manager_destroy(wireguard_manager_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->created) {
        esp_wireguard_disconnect(&handle->wg_ctx);
    }
    free(handle);
    return ESP_OK;
}

esp_err_t wireguard_manager_get_status(wireguard_manager_handle_t handle,
                                       wireguard_manager_status_t *out_status)
{
    if (!handle || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->created = handle->created;
    out_status->connected = handle->created &&
                            (esp_wireguardif_peer_is_up(&handle->wg_ctx) == ESP_OK);
    handle->connected = out_status->connected;
    strlcpy(out_status->address, handle->address, sizeof(out_status->address));
    strlcpy(out_status->endpoint, handle->endpoint, sizeof(out_status->endpoint));
    out_status->endpoint_port = handle->endpoint_port;
    out_status->make_default = handle->make_default;
    return ESP_OK;
}
