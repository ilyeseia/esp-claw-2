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
 * cap_ssh: a network-reachable SSH console, built on wolfSSH.
 *
 * v1 scope, deliberately narrow:
 *   - Public-key authentication only (no passwords).
 *   - One session at a time.
 *   - Not a proxy for the full esp_console REPL (ask/session/auto/etc.) —
 *     redirecting esp_console's global stdout/stdin per SSH session would
 *     risk corrupting concurrent serial-console output, a real ESP-IDF
 *     footgun. Instead this exposes a small purpose-built shell
 *     (help/list/groups/call <name> <json>) built directly on claw_cap's own
 *     API (claw_cap_call, claw_cap_build_catalog, claw_cap_list_groups) —
 *     the same primitives the real serial console's "cap" command uses.
 *   - No interface restriction: this device's current VPN mode
 *     (tailscale-gateway) runs no local tunnel interface to bind against;
 *     the SSH port listens on all interfaces, same as the existing local
 *     HTTP config API. Security instead comes from public-key auth being
 *     the FIRST authenticated network surface on this device (today's
 *     HTTP config API has none at all).
 *
 * Two tools:
 *   - ssh_configure (root-agent only, NOT reachable over MQTT — same
 *     caller-check as every other ROOT_AGENT_ONLY tool): sets the device's
 *     own SSH host identity key (base64-encoded DER — operator-generated,
 *     e.g. `ssh-keygen -m PEM -t ecdsa -f hostkey && openssl ec -in hostkey
 *     -outform DER | base64 -w0`; DER rather than PEM because wolfSSH's PEM
 *     host-key loading returned WS_UNIMPLEMENTED_E in live testing on this
 *     build — the same "operator provides the private key" precedent
 *     already used by wireguard_configure) and the one authorized client
 *     public key (a standard OpenSSH pubkey line: RSA or ECDSA — NOT
 *     ed25519, see below). Starts the server on first successful call.
 *   - ssh_status: reports configured/running/port. Never returns keys.
 *
 * Client key type: RSA or ECDSA only. This managed wolfSSH build's
 * user_settings.h defines HAVE_ED25519 (for wolfCrypt) but not the three
 * extra macros wolfSSH itself additionally requires to enable ed25519 user
 * auth (WOLFSSL_ED25519_STREAMING_VERIFY, HAVE_ED25519_KEY_IMPORT,
 * HAVE_ED25519_KEY_EXPORT — see wolfssh/internal.h's WOLFSSH_NO_ED25519
 * auto-guard), so it silently auto-disables ed25519 (NameToId() returns
 * ID_UNKNOWN for "ssh-ed25519", rejected before cap_ssh_user_auth() is ever
 * called). Live-verified: this — not a channel/dispatch bug — was the cause
 * of what first looked like a post-auth hang with an ed25519 test key; an
 * ECDSA key authenticates and the shell works normally. Fixing ed25519
 * would mean adding those three defines for both the wolfssl__wolfssl and
 * wolfssl__wolfssh managed components without hand-editing their vendored
 * user_settings.h (e.g. the same idf_component_get_property +
 * target_compile_definitions pattern used transiently for DEBUG_WOLFSSH
 * while diagnosing this) — not done here since RSA/ECDSA already cover it.
 */

/* Registers the cap_ssh group (ssh_configure, ssh_status). */
esp_err_t cap_ssh_register_group(void);

#ifdef __cplusplus
}
#endif
