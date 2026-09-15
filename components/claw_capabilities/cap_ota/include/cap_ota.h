/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Over-the-air firmware update capability (remote flashing over the network /
 * Tailscale).
 *
 * Tools:
 *   - ota_status : report the running partition, firmware version/build and
 *                  whether a dual-OTA layout is present (update possible).
 *   - ota_update : download a firmware image from an https:// URL into the
 *                  inactive OTA slot, set it as the boot partition and reboot.
 *                  Plain http:// is rejected (no separate image signing is
 *                  configured, so TLS is the only integrity/authenticity
 *                  check). Restricted, root-agent only — it replaces the
 *                  running firmware.
 *
 * Requires a dual-OTA partition table (ota_0 + ota_1 + otadata).
 */
esp_err_t cap_ota_register_group(void);

#ifdef __cplusplus
}
#endif
