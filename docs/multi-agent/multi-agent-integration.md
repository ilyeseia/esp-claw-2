# Multi-agent coordination — ESP-Claw + Home Assistant

Tracks the execution of the ESP-CLAW 2 + HOME ASSISTANT brief. See `architecture_report.md` for the
full architecture contract; this file is the live status tracker (brief §8).

## Adaptation note (read this first)

The brief asks for 10 parallel-capable sub-agents. Two real constraints changed the execution model,
both anticipated by the brief's own rules:

1. **The reference repo is tiny** (1,732 lines, 10 files) — Agent 1 was executed directly by the
   Lead Orchestrator instead of dispatched, per brief §3 "do not create unnecessary agents".
2. **Every new capability must touch the same ~5 shared files** (Kconfig, `app_capabilities.c`,
   `app_config.c/.h`, `http_server_config_api.c`) — the exact "core file" collision the brief's own
   §7 warns against ("No two agents should independently rewrite the same core file"). Agents
   2–6's implementation work was therefore executed **sequentially by the Lead Orchestrator**,
   in the dependency order from the architecture doc, rather than as independent parallel
   sub-agents — the Lead Orchestrator reviews its own diff after each phase exactly as §9 requires
   for any sub-agent's output. The build gate (§11), being a deterministic long-running tool
   invocation rather than a judgment task, runs as a background process, not a sub-agent.

This is reported honestly below rather than claimed as "10 agents ran in parallel" when they did not.

## Status table

| Agent | Task | Status | Files owned | Result |
|---|---|---|---|---|
| 1 — Architecture Analyst | Map both repos, produce contract | COMPLETED | `docs/multi-agent/architecture_report.md` | Done — see report |
| 2 — Home Assistant | `cap_home_assistant` component | COMPLETED — build-verified | `components/claw_capabilities/cap_home_assistant/**` | ha_get_entities/ha_get_state/ha_call_service/ha_search_entity |
| 3 — Hardware/GPIO | `cap_hardware` component | COMPLETED — build-verified | `components/claw_capabilities/cap_hardware/**` | gpio_read/gpio_write/sensor_read, name-only addressing, allow-list + hard-denied pin ranges enforced |
| 4 — AI Tools | Register both groups in the existing tool registry | COMPLETED — build-verified | `app_capabilities.c`, `app_config.{c,h}`, `app_claw.h`, `main.c`, `Kconfig`, `idf_component.yml` (shared, single-writer) | No new tool framework introduced; both groups added to both registration tables + full 3-site config copy + component-manager manifest entries |
| 5 — Networking | Timeouts, TLS posture, failure handling | COMPLETED (reviewed inline as part of 2/3) | n/a | See architecture doc §3.3 — `esp_crt_bundle_attach` real TLS validation, 8s HTTP timeout, bounded response buffers (48KB HA / entity cap 40) |
| 6 — Web UI | Config pages for both new groups | COMPLETED — typecheck + production build passed | `frontend_source/src/pages/HomeAssistantPage.tsx`, `HardwarePage.tsx`, plus wiring in `App.tsx`, `Sidebar.tsx`, `state/dirty.ts`, `api/client.ts`, `i18n/en.ts`, `i18n/zh-cn.ts` | Wired into the same `/api/config` groups (`home_assistant`, `hardware`); `pnpm run typecheck` and `pnpm run build` both pass, `dist/index.html.gz` regenerated |
| 7 — Security | Audit + fixes | COMPLETED | n/a (review only, findings applied inline) | See "Security" in `docs/multi-agent/home_assistant_and_hardware.md`. Fixes applied during implementation: memory leak in `cap_ha_call_service_execute`, cJSON/strdup allocator mismatch, missing `#include <stdbool.h>` in `cap_home_assistant.h`, and an out-of-scope helper call (`app_cap_config_bool`) caught by the build gate |
| 8 — Performance | Flash/RAM report | COMPLETED — measured from a real build | n/a | App image 2,954,137 bytes total; flash app partition 44% free (0x500000 partition, 0x22ebf0 free); IRAM 100% used (pre-existing, not caused by this change — no IRAM-tagged code was added); DIRAM 48.32% used. No isolated before/after delta was measured (would require a second clean build without these two capabilities) — treat this as the absolute measured size, not an attributed delta |
| 9 — QA / build gate | `idf.py build` | **PASSED** | n/a | `idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1` → `idf.py reconfigure` → `idf.py build`, exit code 0, zero warnings in any touched file. Three real bugs were caught and fixed by this gate (see below) |
| 10 — Documentation | README + docs update | COMPLETED (docs/, not README.md — see note) | `docs/multi-agent/home_assistant_and_hardware.md` | `README.md` documents no individual capability today (not MQTT, VPN, SSH, etc.) so adding a Home-Assistant/Hardware section there would be inconsistent with its existing scope; feature documentation instead lives in `docs/multi-agent/home_assistant_and_hardware.md` |

### Bugs the build gate actually caught (honest record, not rounded up)

1. `cap_home_assistant.h` used `bool` in a function signature without `#include <stdbool.h>` —
   compile error, fixed by adding the include.
2. `app_capabilities.c`'s new `app_cap_prepare_home_assistant()` called `app_cap_config_bool()`,
   a `static` helper that only exists (and is only *defined further down the file*) inside the
   `#if CONFIG_APP_CLAW_CAP_MQTT` block — invisible/unavailable at the point of use. Fixed by
   using the same inline `strcmp(..., "true") == 0 || strcmp(..., "1") == 0` pattern this file
   already uses for `vpn_enabled`/`ssh_enabled`.
3. `components/common/app_claw/idf_component.yml` — a **third** capability-registration file
   (beyond Kconfig/app_config/app_capabilities/http_server_config_api) that declares every
   `cap_*` component as a Kconfig-gated local path dependency for the ESP-IDF component
   manager — was never updated. Without it, `cap_hardware` and `cap_home_assistant` were
   silently absent from CMake's resolved component list entirely (confirmed by diffing the
   `-- Components:` line before/after the fix), even though their Kconfig options existed and
   `app_capabilities.c` referenced their headers. This is now recorded as a fourth shared file
   in the file-ownership model for any future capability addition to this codebase.

Update this table's Status column as each phase actually finishes; do not mark COMPLETED before the
corresponding diff has been reviewed.
