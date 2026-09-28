# Home Assistant + Hardware capabilities

Documents what was actually built and build-verified in this integration (see
`architecture_report.md` for the design rationale and `multi-agent-integration.md` for
the execution/status tracker). Both capabilities are optional, Kconfig-gated, and off by
default at the tool level until configured.

## Home Assistant (`cap_home_assistant`)

Talks to a real Home Assistant instance over its REST API
(https://developers.home-assistant.io/docs/api/rest/) using a long-lived access token.

**Tools exposed to the AI** (family `home_assistant`):
- `ha_get_entities` — `GET /api/states`, optionally filtered by domain, capped at 40 entities
  in the response.
- `ha_get_state` — `GET /api/states/<entity_id>`.
- `ha_call_service` — `POST /api/services/<domain>/<service>` with an optional JSON body.
- `ha_search_entity` — substring search over the entity list.

**Setup**: Web UI → "Home Assistant" page (or `/api/config` group `home_assistant`) — enable
the integration, set the base URL (e.g. `http://homeassistant.local:8123`, no trailing
slash) and a long-lived access token generated from your Home Assistant user profile.
Takes effect after a restart (the capability's config is applied once at boot, not hot-reloaded).

**Security**:
- Entity IDs, domains, and service names supplied by the model are validated against Home
  Assistant's own identifier charset before being placed in a URL path — never accepted as
  a full arbitrary URL.
- Real TLS certificate validation via `esp_crt_bundle_attach` whenever the base URL is `https://`.
- The token is sent only as a Bearer header and is never logged; request logging includes
  only the path and HTTP status.
- Every HTTP call has an 8-second timeout, and response bodies are size-capped (48KB) to
  bound memory use.
- Like every other secret in this build (LLM API keys, MQTT password, WireGuard private key),
  the token is stored in the same plaintext `app_config_t`/NVS blob and is therefore readable
  in full via an authenticated `GET /api/config` — this is an existing, accepted limitation of
  this firmware's config system, not something newly introduced here. Only expose this device's
  Web UI on a trusted network.

## Hardware / GPIO (`cap_hardware`)

**Tools exposed to the AI** (family `hardware`): `gpio_read`, `gpio_write`, `sensor_read`.

**Setup**: Web UI → "Hardware" page (or `/api/config` group `hardware`) — define a JSON array
of named devices in `hw_pins`:
```json
[
  {"gpio": 5, "name": "water_pump", "type": "switch", "mode": "output", "allowed": true},
  {"gpio": 34, "name": "soil_moisture", "type": "sensor", "mode": "analog", "allowed": true}
]
```
Takes effect after a restart.

**Security model — name-only addressing**: the AI is never given a raw GPIO number. Every
tool call takes a `name`, which is resolved server-side against the operator-defined list.
- `gpio_write` additionally requires the matched entry to have `mode == "output"` and
  `allowed == true`.
- A small set of pins that are deterministically unsafe to repurpose on the compiled target
  (SPI flash / PSRAM pins — GPIO 6–11 plus 1 and 3 on ESP32, GPIO 26–32 on ESP32-S3) is
  refused unconditionally, regardless of what the operator's `hw_pins` list says. This is a
  safety net on top of, not a substitute for, reviewing the configured list.
- Analog reads use the ESP-IDF `esp_adc` oneshot driver on ADC unit 1 only; ADC2 is rejected
  because it conflicts with Wi-Fi on ESP32/ESP32-S3.

## What was verified, and how

- **Build-verified**: both components compile and link as part of the full firmware image
  for the `esp32_S3_DevKitC_1` board (ESP-IDF v5.5.4, `idf.py build`, exit code 0, no warnings
  in the touched files). The Web UI pages type-check (`tsc --noEmit`) and build (`vite build`)
  cleanly, and the resulting `dist/index.html.gz` was regenerated and is part of this change.
- **Not verified**: no physical Home Assistant instance or physical GPIO/sensor hardware was
  exercised — this integration has not been tested against real hardware or a live Home
  Assistant server. Treat the tool behavior as build/static-analysis-verified only until it
  has been run on an actual board.

## Troubleshooting

- **Home Assistant tools return "not configured"**: check that `ha_enabled` is `true` and both
  `ha_base_url` and `ha_token` are non-empty (see `cap_ha_not_configured()` in
  `cap_home_assistant.c`) — and that the device was restarted after saving.
- **`gpio_write`/`sensor_read` refuses a name**: the name must appear in `hw_pins` with
  `allowed: true` (and `mode: "output"` for writes); check the JSON was saved without a
  trailing-comma or quoting error (the Web UI's Hardware page validates the JSON shape before
  allowing a save).
- **A configured GPIO is always refused**: it may be one of the hard-denied flash/PSRAM pins
  for this chip target — pick a different GPIO.
