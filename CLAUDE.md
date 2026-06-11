# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Beacon is a 2.16" AMOLED touch desk device (Waveshare ESP32-S3-Touch-AMOLED-2.16) showing Claude/Codex AI usage, markets, weather, and a "Coding Buddy" for approving Claude Code tool-permission prompts. Two parts:

- `firmware/` — device firmware (PlatformIO, Arduino-ESP32 core, LVGL 8.4)
- `hub/` — macOS menubar companion (Swift 6, SwiftPM, no third-party deps)

**Two-plane architecture:** the device fetches public data (weather, finance, time) directly over WiFi; private data (AI usage, permission prompts) comes from the macOS hub over BLE. Credentials never reach the device — the hub normalizes usage to percentages + epoch timestamps.

## Commands

### Firmware (`cd firmware`)

```bash
~/.beacon-pio/bin/pio run                     # compile for ESP32-S3
~/.beacon-pio/bin/pio run -t upload           # flash device
~/.beacon-pio/bin/pio device monitor          # serial @115200 (Ctrl-] to exit)
~/.beacon-pio/bin/pio test -e native          # all host unit tests
~/.beacon-pio/bin/pio test -e native -f "*test_theme*"   # single test folder
```

Tests run in the `native` env (Unity framework, pure C++ — no Arduino/LVGL; device-only code is guarded with `#if !BEACON_NATIVE`). Each `test/test_<unit>/` folder is one suite.

PlatformIO must run from the dedicated Python 3.13 venv at `~/.beacon-pio` (pioarduino `55.03.35` requires Python 3.10–3.13; Homebrew's `platformio` runs on 3.14 and is rejected). The venv carries `platformio` + `pyyaml`. Setup in `firmware/README.md`.

### Hub (`cd hub`)

```bash
swift build            # compile check
swift test             # unit tests (BeaconHubKit)
./build-app.sh run     # build, ad-hoc sign, launch with logs
./build-app.sh release # release bundle (same as CI)
```

Requires Xcode 16+ and `jq` (hooks installer).

## Pinned Toolchain — Do Not Bump Without a Spike

- **Arduino-ESP32 core 3.3.5** (pioarduino platform `55.03.35`): core 3.3.6+ changed `spiFrequencyToClockDiv()` signature and breaks GFX 1.6.4.
- **GFX_Library_for_Arduino 1.6.4**: CO5300 + QSPI support.
- **LVGL 8.4.0**: needs `lv_conf.h` at compile time; draw buffers live in PSRAM (`-DBEACON_LVGL_PSRAM`) — internal SRAM collapsed under BLE+TLS load.
- **BLE**: use only the core `BLE*` wrapper (NimBLE-backed in pioarduino). Do NOT add the separate NimBLE-Arduino (h2zero) library — 1.4.x crashes on core 3.x.

## Architecture

### Firmware layers (`firmware/src/`)

- `core/` — `DataStore` (thread-safe pub/sub for all domain records), `records.h` (frozen record schema with per-source state: LOADING/LIVE/STALE/OFFLINE/ERROR/...), `hub_proto.cpp` (BLE frame parser), WiFi multinetwork, timezone map.
- `fetch/` — API parsers (Open-Meteo weather, Yahoo/Binance finance, IP geolocation). Run on Core 0 timers, write snapshots to DataStore.
- `hal/` — hardware init: AXP2101 power, CO5300 display, CST92xx touch, QMI8658 IMU, PCF85063 RTC.
- `ui/` — LVGL: `carousel.cpp` (swipe nav + screen lifecycle), `theme.h` (token struct, 7 themes), `gauge.cpp` (one component, token-switched into 7 visual styles), `screens/` (5 screens, each exposing build/update/destroy), `screens/views/` (per-theme per-screen layouts, 35 views).

**Threading:** Core 0 = WiFi/BLE/timers/HTTP; Core 1 = LVGL render only. All inter-core communication goes through DataStore snapshots — never block the UI loop.

### Hub targets (`hub/Sources/`)

- `BeaconHubKit/` — pure, host-testable logic: BLE frame codec, usage normalization (Claude statusline + Codex endpoint to unified `{h5, d7, pct, reset}`), pairing state machine, hooks detection.
- `beacon-hub/` — menubar agent: CoreBluetooth central, Claude Code hooks bridge (PermissionRequest + statusline shim on `127.0.0.1:8765`), hooks installer (`jq` merge into `~/.claude/settings.json`), menubar/first-run UI.

### Data flow

Usage: hub polls Claude (Keychain token) + Codex (`~/.codex/auth.json`) → normalizes → BLE status frame → device `onFrame()` → DataStore → screens.

Permissions: Claude Code `PermissionRequest` hook → hub (HTTP) → BLE frame with `buddy.prompt` → device Approve/Deny tap → BLE command back → hub answers the hook. Device-side timeout 25 s; fail closed (deny).

**BLE protocol:** Nordic UART service, bonded + LE Secure Connections, newline-delimited JSON frames (MTU 247). Device is peripheral, hub is central. The frame schema is frozen in `hub/CONTRACT.md` — keep `core/hub_proto.cpp`, `BeaconHubKit/Protocol.swift`, and their tests in sync with it.

`HubLink` is the transport-agnostic interface screens depend on (BLE today, LAN-WebSocket planned) — don't couple UI to BLE directly.

## Hardware Constraints

- Display is **466×466** (not the advertised 480), rounded-square: inset all content ≥40 px from edges; brightness via DCS command 0x51 (no PWM).
- **Strict boot order:** I2C → AXP2101 rails (ALDO1-4 @3.3V) → display → panel DCS → LVGL → WiFi → BLE. AXP2101 before display, or the panel stays dark after power cycles.
- Memory is tight: ~53 KB min free internal heap under BLE+TLS+LVGL. One theme's styles/fonts resident at a time.
- TLS uses bundled CA roots; never `setInsecure()`.
- CoreBluetooth cannot remove OS-level bonds — stuck bonds (e.g., after re-flash) require manual forget in macOS Bluetooth settings.

## Configuration & Secrets

- Firmware has **no compiled-in secrets**: WiFi via on-device `Beacon-setup` captive portal; user settings (tickers, theme, brightness, location) in NVS; defaults in `config/tickers.h`.
- Hub reads the Claude token from Keychain (`Claude Code-credentials`) and Codex from `~/.codex/auth.json`.

## Where Documents Go

Always reuse the existing `docs/` structure — never create new top-level doc directories (no `docs/superpowers/`, `docs/<agent-name>/`, etc.):

- `docs/specs/` — design documents (`YYYY-MM-DD-<topic>-design.md`)
- `docs/plans/` — implementation plans (`<issue#>-<topic>.md` or `YYYY-MM-DD-<phase>-plan.md`)
- `docs/research/` — research notes
- `docs/spikes/` — throwaway hardware/coexistence proofs

This applies to skills and subagents too: if a workflow (e.g., superpowers writing-plans) defaults to its own output location, redirect it to the directories above.

## Authoritative Docs

- `DESIGN.md` — visual system: theme tokens, 7-theme catalog, screen states, safe-area. Read before any UI work.
- `docs/tech.md` — technical constitution: NFRs, hardware budgets, bring-up sequence, frozen contracts.
- `hub/CONTRACT.md` — frozen BLE protocol schema and hub-side policies.
- `docs/prd.md` — functional roadmap and phased acceptance.
- `CONTRIBUTING.md` — contribution conventions: Conventional Commits (`type(scope): subject`, lowercase imperative subject; scopes `firmware`/`hub`/`docs`/`ci`), PR titles follow the same rules (they become the squash-merge commit; link issues in the body, not the title), branch naming `<type>/<issue#>-<kebab-summary>`, issue labeling, and per-component semver tags (`firmware-vX.Y.Z` / `hub-vX.Y.Z`).
