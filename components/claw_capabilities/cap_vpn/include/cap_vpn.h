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

/*
 * Tailscale "gateway model" (Option C) VPN integration.
 *
 * ESP32 cannot run Tailscale natively, so the device stays a normal LAN client
 * and a Tailscale subnet-router on the LAN bridges it to the tailnet. This
 * capability holds the gateway/tailnet settings and exposes a `vpn_status`
 * tool that probes reachability of a tailnet host (DNS + TCP) so the agent and
 * the Web UI can confirm the route is up. No WireGuard runs on the device.
 */
typedef struct {
    bool        enabled;      /* gateway integration active */
    const char *gateway;      /* subnet-router / gateway host or IP on the LAN (informational) */
    const char *test_host;    /* tailnet host to probe, e.g. "searxng.tailXXXX.ts.net" */
    uint16_t    test_port;    /* TCP port to probe, 0 => 80 */
} cap_vpn_config_t;

/* Apply configuration. Safe to call again to update settings. */
esp_err_t cap_vpn_set_config(const cap_vpn_config_t *config);

/*
 * Optional persistence hook. When set, the (root-agent-only) vpn_configure tool
 * writes the applied settings through this callback so they survive a reboot.
 * The app wires this to its NVS-backed config store.
 */
typedef esp_err_t (*cap_vpn_persist_fn)(const cap_vpn_config_t *config, void *user_ctx);
esp_err_t cap_vpn_set_persist_provider(cap_vpn_persist_fn persist, void *user_ctx);

/* Register the cap_vpn group (vpn_status, vpn_configure). */
esp_err_t cap_vpn_register_group(void);

#ifdef __cplusplus
}
#endif
