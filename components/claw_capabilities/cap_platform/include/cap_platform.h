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
 * cap_platform: an authenticated, deterministic trigger path for RESTRICTED/
 * ROOT_AGENT_ONLY capabilities (e.g. ota_update, mqtt_configure,
 * vpn_configure) over the MQTT command bridge, where those capabilities are
 * normally unreachable (claw_cap_call denies ROOT_AGENT_ONLY tools for the
 * bridge's CLAW_CAP_CALLER_SUB_AGENT caller — see cap_mqtt.c).
 *
 * Two tools:
 *   - platform_configure (root-agent only): sets the shared secret used to
 *     verify tokens. Intended to be called once, locally (console/WebIM/
 *     Telegram-as-root) — never over MQTT, since it is itself
 *     ROOT_AGENT_ONLY and therefore unreachable from the bridge, by design.
 *   - platform_exec (RESTRICTED, NOT root-agent-only — reachable over MQTT):
 *     accepts a single signed token and, only after verifying it, invokes
 *     the token's target capability internally with root-agent authority.
 *     Authorization for this path comes from the signature, not the caller.
 *
 * Token shape (JWT-HS256-shaped, not literally JWT): `base64url(payload) +
 * "." + base64url(HMAC-SHA256(ASCII bytes of that base64url segment,
 * secret))`, where `payload` is the JSON object
 * `{device_id, capability, input, issued_at, expires_at, nonce}`. Signing
 * over the base64url TEXT (not the decoded JSON bytes) avoids any
 * JSON-canonicalization ambiguity between the issuer (this platform's
 * TokenService) and this verifier — both only need to agree on one
 * base64url encoding of one JSON string, not on a byte-identical
 * re-serialization.
 *
 * The device never generates or knows this secret on its own — an operator
 * generates it and pushes it via platform_configure. This mirrors the
 * design note "provisioned once, out-of-band, over a trusted local
 * channel — never over MQTT."
 */

/* Registers the cap_platform group (platform_configure, platform_exec). */
esp_err_t cap_platform_register_group(void);

#ifdef __cplusplus
}
#endif
