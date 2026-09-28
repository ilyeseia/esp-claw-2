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
 * cap_hardware: a small, config-driven GPIO/sensor abstraction.
 *
 * SECURITY (see docs/multi-agent/architecture_report.md §3.1, and the reference-repository finding
 * R3 it exists specifically to avoid): the LLM is never given a raw GPIO number to control. Every
 * tool here (`gpio_read`, `gpio_write`, `sensor_read`) takes a `name` — a logical device name that
 * an operator defined in advance via the `hw_pins` config field (Web UI "Hardware" page). The
 * mapping from that name to a physical pin, its direction and whether it may be written at all is
 * entirely operator-controlled configuration; the model can only ever reference a name that already
 * exists in that list, with the permissions ("allowed") the operator gave it.
 *
 * `hw_pins` shape (JSON array), one entry per logical device:
 *   {"gpio": 5, "name": "water_pump", "type": "switch"|"sensor"|"input"|"output",
 *    "mode": "input"|"output"|"analog", "allowed": true}
 * `gpio_write` additionally requires `mode=="output"` and `allowed==true`.
 *
 * On top of the operator's own list, a small set of pins that are deterministically unsafe to
 * repurpose on the compiled target (SPI flash/PSRAM pins) is always refused, regardless of config —
 * see `cap_hardware_pin_hard_denied()` in the .c file. This is a safety net, not a substitute for
 * reviewing the configured list: everything else the operator marks `"allowed": true` is honoured.
 */

/* Registers the cap_hardware group (gpio_read, gpio_write, sensor_read). */
esp_err_t cap_hardware_register_group(void);

/* Sets the current hw_pins JSON array (from app_claw_config_t.hw_pins). Copied internally. */
esp_err_t cap_hardware_set_pins(const char *pins_json);

#ifdef __cplusplus
}
#endif
