/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Network (static IP) configuration capability.
 *
 * Tools:
 *   - network_status    : report the current STA IP / gateway / netmask / DNS
 *                         and whether DHCP or a static IP is in use.
 *   - network_configure : set a static IPv4 (or switch back to DHCP) and persist
 *                         it. Root-agent only + restricted. Changing the device IP
 *                         drops the current connection, so it is persisted and
 *                         applied on the next reboot rather than live.
 */
typedef struct {
    bool        use_static;
    const char *ip;
    const char *gateway;
    const char *netmask;
    const char *dns;
    const char *dns2;
} cap_netcfg_config_t;

typedef esp_err_t (*cap_netcfg_persist_fn)(const cap_netcfg_config_t *config, void *user_ctx);
esp_err_t cap_netcfg_set_persist_provider(cap_netcfg_persist_fn persist, void *user_ctx);

esp_err_t cap_netcfg_register_group(void);

#ifdef __cplusplus
}
#endif
