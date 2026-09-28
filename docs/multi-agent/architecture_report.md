# Architecture Report — ESP-Claw + Home Assistant Integration

**Author:** Agent 1 (Architecture Analyst), executed directly by the Lead Orchestrator.
**Adaptation note (documented per brief §3 "do not create unnecessary agents" and §6 "adapt to the
actual project structure"):** the reference repository turned out to be a single 1,732-line Arduino
sketch across 10 files. Dispatching a fresh sub-agent to re-derive that from scratch would have cost
more in coordination overhead than reading it directly, so the Lead Orchestrator performed this
analysis in-session, against the real source of both repositories (per the `esp-claw-iot` skill's
rule: never assert a fact without checking the file).

Status: read-only analysis. No production source was modified to produce this report.

---

## 1. Target: `esp-claw-2` (verified against this clone, HEAD `77d10dc`)

ESP-IDF native (C, FreeRTOS), NOT Arduino. Modular capability architecture:

- **`claw_cap` runtime** (`components/claw_modules/claw_cap/`): every AI-callable tool is a
  `claw_cap_descriptor_t` (`id`, `family`, `kind`, `cap_flags`, `input_schema_json`, `execute()`),
  grouped into a `claw_cap_group_t`, registered idempotently. `cap_flags` include
  `CALLABLE_BY_LLM`, `ROOT_AGENT_ONLY` (enforced), `RESTRICTED` (**declared but not enforced** —
  verified in `claw_cap_authorize_llm_tool_locked()`).
- **`claw_core`**: the real agent loop — multi-turn LLM tool-calling already exists
  (`claw_cap_build_llm_tools_json()` + `claw_cap_call()`). There is **no bracket-text-parsing tool
  mechanism** anywhere in this codebase; every existing capability goes through the same registry.
- **Capabilities today** (`components/claw_capabilities/`): `cap_agent_mgr`, `cap_boards`, `cap_cli`,
  `cap_files`, `cap_http_request` (generic outbound HTTP, gated by a configured allow-list),
  `cap_im_local`, `cap_im_platform`, `cap_llm_config`, `cap_llm_inspect`, `cap_lua`, `cap_mcp_bridge`,
  `cap_mcp_client`, `cap_mcp_server`, `cap_mqtt`, `cap_netcfg`, `cap_ota`, `cap_platform`,
  `cap_router_mgr`, `cap_scheduler`, `cap_session_mgr`, `cap_skill_mgr`, `cap_ssh`, `cap_system`,
  `cap_vpn`, `cap_web_search`. **No `home_assistant` or generic `hardware`/`gpio` capability exists
  yet** — clean slate, no duplication risk.
- **Configuration system**: one shared `app_claw_config_t` struct
  (`components/common/app_claw/include/app_claw.h`), fields declared with `APP_CONFIG_FIELD` in
  `application/edge_agent/components/app_config/app_config.c` (NVS-backed, default macros), exposed
  to the Web UI via a `CONFIG_FIELD("group", field)` table in
  `application/edge_agent/components/http_server/http_server_config_api.c`
  (`GET/POST /api/config`). **Every capability's own config lives as fields on this one struct** —
  there is no second, competing configuration system anywhere, and none should be introduced.
- **Capability registration**: one table in `components/common/app_claw/app_capabilities.c`
  (`app_capability_group_entry_t[]`), each entry gated by its own `CONFIG_APP_CLAW_CAP_XXX` Kconfig
  bool (`components/common/app_claw/Kconfig`), with an optional `prepare` step (build a config
  struct, e.g. `cap_vpn_config_t`) and a `register` step. `enabled_cap_groups` (default empty =
  "all registered groups enabled") lets an operator further restrict at runtime.
- **Networking**: `mqtt_manager` (real broker client, retained status + LWT — three topic leaves
  only: `status`/`command`/`response`), `cap_netcfg`, `cap_vpn` (Tailscale/WireGuard),
  `esp_https_ota` + `esp_crt_bundle_attach` (**real CA validation**, never `setInsecure()`-style
  bypass). `cap_http_request` already establishes the pattern for "call an external HTTP endpoint
  safely": a configured allow-list, `esp_http_client`, an explicit `timeout_ms`, and a JSON result
  returned even on failure — this is the correct template for a new HA-calling capability, not a
  new HTTP client implementation.
- **Hardware today**: no generic `cap_gpio`/`cap_hardware` LLM tool exists. GPIO is reached only
  through board-specific Lua drivers (`lua_driver_gpio`, `_adc`, `_i2c`, `_pcnt`, `_rmt`, `_touch`,
  `_uart`) inside the separate `cap_lua` scripting capability, or via board peripheral wiring
  (`boards/<vendor>/<board>/`). There is **no existing "AI directly writes an arbitrary GPIO number"
  tool** — which is good, because that is exactly the antipattern found in the reference (§2 below)
  and exactly what this integration must not introduce.
- **Web UI**: `application/edge_agent/components/http_server/frontend_source` — a real React/TS
  app built with pnpm, one page per config group (`MqttPage.tsx`, `McpPage.tsx`, ...), all reading
  the same `CONFIG_FIELD` groups from `/api/config`. A new config group gets a new page in this same
  structure, not a second UI.
- **Security posture already in place, worth preserving**: `cap_ota` requires HTTPS + validates the
  image hash before activating it; secrets already live in the same NVS-backed config struct as
  every other field (an existing, accepted, documented tradeoff — `/api/config` returns them in
  plaintext, same as `mqtt_password`/`llm_api_key` today; not something this integration should try
  to silently "fix" as a side effect, since that is unrelated-component scope creep, but also not a
  precedent to make *worse*).
- **Out of scope for this task, noted but not touched**: `CLAW_CAP_FLAG_LOCAL_ONLY` /
  `remote_origin` (a remote-origin capability hardening applied in a *different* branch/session) is
  **not present in this clone**, and `mcp_enabled` defaults to `"true"` here. Neither is related to
  Home Assistant/hardware and neither is modified by this work, per the brief's own rule against
  touching unrelated components.

## 2. Reference: `espclaw-homeAssistante` (verified against `1cec949`, PlatformIO/Arduino)

**Different framework entirely**: PlatformIO + Arduino core (ESP8266/ESP32), a single `loop()`-style
sketch (`src/main.cpp`, 289 lines) with `ConfigManager` (Arduino `Preferences`/NVS wrapper),
`LLMClient` (533 lines, hand-rolled raw-socket HTTPS to OpenAI/Anthropic/Gemini), `WebUI`, `TelegramBot`,
`Logger`. **None of this C++ is portable as-is** into an ESP-IDF component — it must be
reimplemented against `claw_cap`/`esp_http_client`, using only the *ideas*, per the brief's own
instruction not to copy the reference.

### 2.1 Home Assistant integration (the idea worth keeping)
Config: one HA token (`haToken`) + a JSON array (`haUrls`) of **manually pre-configured, per-entity
full URLs** with a human description and an `isSensor` flag. Every chat turn,
`LLMClient::getHAInstructions()` **synchronously fetches every configured sensor's state over
HTTP**, serially, before the LLM is even called, and injects the results as plain text into the
system prompt. Actions are invoked by the LLM emitting `[HA_CALL: <index>]` in its free-text reply;
`main.cpp::executeTools()` string-scans the reply for that bracket and issues a blind
`POST <url-at-that-index>` with body `{}`.

**What to keep as an idea:** Home Assistant's REST API (`/api/states`, `/api/states/<entity_id>`,
`/api/services/<domain>/<service>`) with a long-lived Bearer token is the right integration surface,
and per-entity human descriptions are a reasonable way to help the LLM. **What not to copy:**
static per-entity URL configuration (does not scale, is not "real" dynamic entity discovery),
serial blocking pre-fetch of every sensor on every turn (a real latency/availability risk — see
§2.3), and bracket-text tool execution (bypasses every safety mechanism `claw_cap` already has).

### 2.2 GPIO / hardware control (the concrete antipattern to avoid)
`executeTools()` parses `[GPIO_ON: <n>]` / `[GPIO_OFF: <n>]` directly out of the LLM's free-text
reply and calls `pinMode(n, OUTPUT); digitalWrite(n, HIGH/LOW)` **with zero validation of `n`** — no
allow-list, no bounds check, no distinction between a user-wired relay pin and a strapping/flash
pin. The system prompt tells the LLM which pins exist via a free-English-text field
(`USER_PINS`/`userPins`), but nothing on the device enforces it. **This is precisely the
"unrestricted GPIO access" the brief's Agent 3 charter prohibits by name**, and is the single most
important thing this integration must not reproduce.

### 2.3 Security findings in the reference (do not carry these into esp-claw-2)
| # | Finding | Evidence |
|---|---|---|
| R1 | **TLS certificate validation is disabled even on ESP32** (not just the memory-constrained ESP8266) for every LLM provider call. | `LLMClient::askOpenAI/askAnthropic/askGemini` all call `client.setInsecure()` unconditionally; only the buffer-size tweak is `#if ESP8266`. |
| R2 | **API keys/tokens are logged in plaintext** as part of a "curl command" debug trace. | `curlCmd += "Authorization: Bearer " + OPENAI_API_KEY; appLogger.log(curlCmd);` in all three provider paths. |
| R3 | **Unrestricted GPIO write from LLM-controlled text**, no allow-list, no pin validation. | `main.cpp::executeTools()`, see §2.2. |
| R4 | Home Assistant action calls are **index-based, not entity/service-based**, and blindly POST an empty body — no service-call schema, no response validation. | `executeTools()`'s `HA_CALL` branch. |
| R5 | Every chat turn does N **synchronous, serial, unbounded-timeout** HTTP GETs (one per configured sensor) before the LLM is even contacted. | `getHAInstructions()`. |
| R6 | Single shared Telegram-chat-id check is the *only* authorization boundary; once passed, free-text LLM output has unmediated GPIO + HA POST authority. | `onTelegramMessage()` + `executeTools()`. |

None of R1–R6 exist in `esp-claw-2` today, and this integration is designed specifically not to
introduce them (see the architecture contract, §3).

## 3. Architecture contract for this integration

**Principle:** Home Assistant and hardware become two new `claw_cap` capability groups, reachable
through the *existing* agent tool-calling loop — no second agent runtime, no second config system,
no second HTTP client, no second Web UI.

```
components/claw_capabilities/
├── cap_hardware/            (NEW — Agent 3: GPIO/ADC abstraction, config-driven allow-list)
│   ├── include/cap_hardware.h
│   └── src/cap_hardware.c
└── cap_home_assistant/      (NEW — Agent 2: HA REST client as claw_cap tools)
    ├── include/cap_home_assistant.h
    └── src/cap_home_assistant.c
```

Shared/core files every new capability must touch (single-writer — see §4 File Ownership):
`components/common/app_claw/Kconfig`, `.../app_capabilities.c`,
`application/edge_agent/components/app_config/{app_config.c,include/app_config.h}`,
`components/common/app_claw/include/app_claw.h`,
`application/edge_agent/components/http_server/http_server_config_api.c`,
`application/edge_agent/components/http_server/frontend_source/src/**` (new pages only).

**Correction found by the build gate, not by inspection**: a fifth shared file exists —
`components/common/app_claw/idf_component.yml` — the ESP-IDF component-manager manifest that
declares every `cap_*` component as a Kconfig-gated local path dependency. It was missed on the
first pass here; omitting it doesn't produce a compile error, it silently excludes the new
component from the build's resolved component list entirely. See
`docs/multi-agent/multi-agent-integration.md` "Bugs the build gate actually caught" for the
full account. Any future capability addition to this codebase must update this file too.

### 3.1 Data model (hardware capability map — matches the brief's own example)
Stored as one JSON-array config field (`hw_pins`, on the shared config struct, exactly like every
other field — no second config system):
```json
[{"gpio": 5, "name": "water_pump", "type": "switch", "mode": "output", "allowed": true}]
```
`gpio_read`/`gpio_write` only accept a pin that appears in this map **and** is not on a
chip-family hard deny-list (flash/PSRAM/strapping pins) — validated inside `execute()`, mirroring
`cap_http_request`'s allow-list pattern. The AI never sees or controls a pin outside this map.

### 3.2 AI tools (Agent 4 wires these into the existing registry — no new tool framework)
```
ha_get_entities   — GET  {ha_base_url}/api/states                (list, optionally domain-filtered)
ha_get_state      — GET  {ha_base_url}/api/states/{entity_id}
ha_call_service   — POST {ha_base_url}/api/services/{domain}/{service}  (JSON body)
ha_search_entity  — client-side substring filter over ha_get_entities
gpio_read         — reads a configured pin
gpio_write        — writes a configured pin (bool), only if "mode":"output" and "allowed":true
sensor_read       — reads a configured analog/sensor-typed pin
```
(A `device_info`-style tool was considered but dropped: `cap_system` already exposes
`get_system_info`, and duplicating it would violate the brief's "never duplicate ... the
configuration system" spirit for the sake of a redundant tool name.)
All flagged `CLAW_CAP_FLAG_CALLABLE_BY_LLM` (normal agent reachability, same trust tier as
`cap_http_request`/`get_system_info` today) — **not** `ROOT_AGENT_ONLY` (these are meant to be used
in normal conversation, exactly like the brief's feature list implies), and their safety comes from
the allow-list + schema validation inside `execute()`, not from caller-identity gating. This
mirrors the documented, reviewed design of `cap_http_request`, not a new trust model.

### 3.3 Networking rules (Agent 5)
- `esp_http_client` with an explicit `timeout_ms` (default 5 s) on every HA call — a slow/offline
  Home Assistant must never hang the agent loop. Config timeouts, connection failures and non-2xx
  HTTP codes all return a structured `{"ok":false,"error":"..."}` result, never a hang or a crash.
- HA runs on the local network and is commonly plain `http://` (no LAN CA) — both `http://` and
  `https://` are accepted for `ha_base_url`; when `https://` is used, certificate validation is
  real (`esp_crt_bundle_attach`), matching `cap_ota`. This is **not** the reference's TLS bypass
  (R1): here, `http://` is an explicit, user-chosen scheme for a same-LAN service, never a silently
  disabled certificate check on an `https://` connection.
- The HA token is never logged (no equivalent of R2's curl-trace-with-secret).
- No sensor is pre-fetched outside of an explicit tool call — no equivalent of R5.

### 3.4 Web UI (Agent 6)
One new config group/page (`home_assistant`: `ha_enabled`, `ha_base_url`, `ha_token`) and one new
group/page (`hardware`: `hw_pins` JSON editor), following the exact same `CONFIG_FIELD` +
React-page pattern as every existing group. No second configuration endpoint, no second storage.

## 4. File ownership (adapted: sequential single-writer, see coordination doc for why)
| Owner (domain) | Paths |
|---|---|
| Agent 2 — Home Assistant | `components/claw_capabilities/cap_home_assistant/**` |
| Agent 3 — Hardware/GPIO | `components/claw_capabilities/cap_hardware/**` |
| Agent 4 — AI Tools | registration lines inside `app_capabilities.c` for the two groups above (no new framework file) |
| Agent 5 — Networking | reviewed as part of Agent 2/3's `execute()` implementations (timeouts, TLS) — no separate files |
| Agent 6 — Web UI | `application/edge_agent/components/http_server/frontend_source/src/pages/HomeAssistantPage.tsx`, `HardwarePage.tsx`, plus their registration in the app's page list |
| Shared core files (Kconfig, app_config.*, http_server_config_api.c, idf_component.yml) | Single-writer: Lead Orchestrator, once per file, after both capabilities' field lists are final |

## 5. Dependency order actually followed
Architecture (this doc) → `cap_hardware` (no dependency) → `cap_home_assistant` (no dependency on
hardware) → shared-file wiring (Kconfig/app_config/http_server_config_api/app_capabilities/
idf_component.yml, one pass) → Web UI pages → build gate (3 fix iterations — see
`multi-agent-integration.md`) → security self-review → performance notes → docs.

## 6. Build verification
`idf.py build` (ESP-IDF v5.5.4, board `esp32_S3_DevKitC_1`) exits 0 with zero compiler warnings
in every file this integration touched. App image: 2,954,137 bytes total, 44% of the app
partition free. This is a build/static-analysis result only — no physical ESP32-S3 board, Home
Assistant instance, or GPIO/sensor hardware was exercised as part of this work.
