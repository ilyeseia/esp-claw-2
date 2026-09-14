/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin transport wrapper around the trombik/esp_wireguard component (lwIP-based
 * on-device WireGuard tunnel). One peer, one tunnel. The handle owns copies of
 * all config strings because esp_wireguard keeps pointers into the config for
 * the lifetime of the context. A valid system clock (SNTP) and a working
 * network interface are prerequisites for the handshake.
 */
typedef struct wireguard_manager_ctx *wireguard_manager_handle_t;

typedef struct {
    const char *private_key;      /* device private key (base64) */
    const char *address;          /* device tunnel IP, optionally "ip/cidr" e.g. "10.2.0.2/32" */
    const char *peer_public_key;  /* peer/server public key (base64) */
    const char *endpoint;         /* peer host or IP */
    uint16_t    endpoint_port;    /* 0 -> 51820 */
    const char *preshared_key;    /* optional (base64), empty/NULL -> none */
    uint16_t    keepalive;        /* persistent keepalive seconds, 0 -> off */
    bool        make_default;     /* route the default gateway through the tunnel (full tunnel) */
} wireguard_manager_config_t;

typedef struct {
    bool     created;
    bool     connected;           /* peer handshake is up */
    char     address[48];
    char     endpoint[128];
    uint16_t endpoint_port;
    bool     make_default;
} wireguard_manager_status_t;

esp_err_t wireguard_manager_create(const wireguard_manager_config_t *config,
                                   wireguard_manager_handle_t *ret_handle);
esp_err_t wireguard_manager_connect(wireguard_manager_handle_t handle);
esp_err_t wireguard_manager_disconnect(wireguard_manager_handle_t handle);
esp_err_t wireguard_manager_destroy(wireguard_manager_handle_t handle);
esp_err_t wireguard_manager_get_status(wireguard_manager_handle_t handle,
                                       wireguard_manager_status_t *out_status);

#ifdef __cplusplus
}
#endif
