# Beacon — Product Requirements (Functional)

> **What this is:** the functional contract for Beacon — *what* it must do, feature by feature, with definitions, acceptance criteria, priorities, and phasing. It is the entry point for any agent or session picking up work.
>
> **Companion docs:** `PRODUCT.md` (strategy/why) · `DESIGN.md` (visual system + theme tokens) · [`tech.md`](tech.md) (technical constitution: how it's built, NFRs, conventions) · `docs/research/` (evidence) · `docs/spikes/` (proven hardware experiments). Where this doc and `tech.md` disagree on *how*, `tech.md` wins; where strategy is unclear, `PRODUCT.md` wins.
>
> **Status (2026-06-08):** **P0 + P1 complete and running on-device** (in `firmware/`). Foundation (bring-up, frozen contracts + theme engine, swipe carousel/35 views, NVS persistence FR-PLAT-3, time service FR-PLAT-8 NTP+RTC, WiFi provisioning FR-SET-1) and the ambient screens (FR-HOME clock+weather, FR-FIN live FX→IDR/BTC/indices, FR-STATE screen-state model) are validated on hardware over real WiFi. Beyond spec: multi-network WiFi (WiFiMulti) + on-device Wi-Fi manager (incl. on-demand add-network, no reboot), IP-based auto-geolocation. **Deferred:** FR-SET-4 (on-device ticker/location editing, SHOULD), FR-PLAT-7 idle dim/sleep. **In progress:** P2 (macOS hub + AI Usage / Coding Buddy over BLE) — code complete and validated on hardware; AI Usage is live on-device over BLE and the buddy permission round-trip is validated (approve/deny honored on the Mac); **P2-0 contract capture done (2026-06-11):** real token-redacted upstream payloads recorded in `hub/CONTRACT.md` §C — the draft shapes matched the live endpoints on every field read (see §7 P2 progress). Conventions: requirement IDs are stable; priority is MoSCoW (MUST/SHOULD/COULD); each requirement has a Phase (see §7).

---

## 1. Vision & scope

**BLUF:** Beacon is a 2.16" AMOLED desk device that, at a glance, shows a developer their Claude Code / Codex usage, live markets, weather/time, and lets them approve Claude tool-permission prompts — without breaking focus on their Mac. It is a **companion to the Mac** (private AI data over BLE) and **independent over WiFi** (public data direct), themeable, gesture-driven.

**In scope (MVP):** five on-device screens (Home, Finance, AI Usage, Coding Buddy, Settings), the 7-theme engine, and a macOS hub app that feeds the two BLE-fed screens.

**Out of scope (MVP):** on-device audio playback (incl. Spotify audio); multi-user; cloud account/sync; companion app for Windows/Linux.

## 2. Users & context

Single owner-user: a developer at their desk. Beacon is **peripheral** — glanced at while working, occasionally acted on (approve a prompt, skip a track), rarely "operated". Every requirement is judged against: *does it deliver value at a glance, or a quick action, without pulling focus from the Mac?*

## 3. Definitions

| Term | Meaning |
|---|---|
| **Device** | The Beacon firmware running on the Waveshare ESP32-S3-Touch-AMOLED-2.16. |
| **Hub** | A macOS menubar companion app (Swift). Holds Claude/Codex tokens; bridges them to the device over BLE. |
| **Plane** | A data path. **Device-direct plane** = device ↔ internet over WiFi+TLS. **Hub plane** = device ↔ Hub over BLE. |
| **Screen** | One full-display view in the swipe carousel. |
| **Theme** | A named set of design tokens (+ optional gauge style); see `DESIGN.md`. Default: Editorial. |
| **Window (usage)** | A rolling rate-limit period. Both Claude and Codex have a **5-hour** and a **7-day** window. |
| **Prompt (buddy)** | A Claude Code tool-permission request the user can Approve/Deny from the device. |
| **Stale** | Data older than its per-source freshness threshold; must be shown as stale, never as live. |

## 4. System overview (functional)

```
   DEVICE-DIRECT PLANE (WiFi+TLS)              HUB PLANE (BLE)
   - Finance (FX/crypto/indices/ETF)           - Claude usage (5h + 7d)
   - Weather + time (NTP)                       - Codex usage (5h + 7d)
                                                - Coding Buddy (state, approve/deny)
                                                - Hub holds Claude/Codex secrets (never reach device)
                         \                      /
                          [ Beacon device ] --- local NVS: settings, theme, WiFi, tickers
```

The ambient screens (device-direct) keep working when the Mac is asleep; the two hub-plane screens degrade gracefully to "hub offline / last synced" (§5.10).

**Secrets boundary:** Claude/Codex tokens are **hub-only and never reach the device**; device-plane integrations (Spotify) hold their own **scoped** tokens (prefer a proxy). The authoritative per-integration table is in [`tech.md`](tech.md) §9.

---

## 5. Functional requirements

Priority = MUST / SHOULD / COULD. Phase per §7.

### 5.1 Device platform & navigation — `FR-PLAT`

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-PLAT-1 | MUST | P0 | On boot, initialize power (AXP2101 rails) before display, bring up the AMOLED + LVGL, and show the last-used screen within ~4s. |
| FR-PLAT-2 | MUST | P0 | Present screens as a horizontal **swipe carousel**; horizontal swipe = prev/next screen, with a position indicator. |
| FR-PLAT-3 | MUST | P0 | Persist last screen, brightness, theme, WiFi creds, and ticker config across reboots (NVS). |
| FR-PLAT-4 | MUST | P0 | All content respects the rounded-display **safe area** (`DESIGN.md`); no critical content or tap target in the corner arcs. |
| FR-PLAT-5 | SHOULD | P3 | Touch gestures beyond swipe: long-press = screen context action; swipe-down = quick brightness. |
| FR-PLAT-6 | SHOULD | P3 | IMU gestures: raise/flick = wake display; shake = dismiss overlay / exit subview. |
| FR-PLAT-7 | SHOULD | P0 | Dim then sleep the display after a configurable idle timeout; wake on touch (and IMU in P3). |
| FR-PLAT-8 | MUST | P0 | **Time service:** NTP sync + PCF85063 RTC for offline persistence + timezone/DST from Settings; expose current time to screens. (Home displays it in P1 via FR-HOME-1.) |
| FR-PLAT-9 | MUST | P0 | Lock `SAFE_INSET` + corner radius against the real panel (cyan-border check), and freeze the flash `partitions.csv` (OTA-reserving) — both block later screen + OTA work. |

**Acceptance:** device boots to a rendered screen; swiping cycles all enabled screens both directions; power-cycling restores prior state; no element is clipped by the rounded corners on hardware; display dims+sleeps after the idle timeout and wakes on touch; time stays correct across a WiFi outage with the right timezone; (P3) long-press, swipe-down, and IMU flick/shake each perform their action.

### 5.2 Theming — `FR-THEME`

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-THEME-1 | MUST | P0 | Render the active theme from design tokens; default **Editorial**. |
| FR-THEME-2 | MUST | P0 | Provide all 7 themes selectable at runtime; selection persists. Canonical IDs (`editorial`,`hud`,`dotmatrix`,`blueprint`,`led`,`oscilloscope`,`analog`) + display names: see `DESIGN.md` theme catalog. |
| FR-THEME-3 | MUST | P0 | A theme switch re-renders the active screen with no reboot and no visual corruption (each theme has bespoke per-screen layouts, so a switch rebuilds the screen from that theme's view). |
| FR-THEME-4 | SHOULD | P0 | Each theme is a bespoke per-screen experience composed from shared tokens + components (styles, the `gauge-style` component, screen-state helpers, per-theme chrome) — distinct layouts, not a recolor. |

**Acceptance:** every screen renders correctly in all 7 themes; switching themes in Settings updates the live UI and survives reboot.

### 5.3 Home — `FR-HOME` (device-direct)

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-HOME-1 | MUST | P1 | Show current time + date, synced via NTP and held by the RTC when offline; correct timezone/DST per Settings. |
| FR-HOME-2 | MUST | P1 | Show current weather: temperature, humidity, condition, for the configured location (Open-Meteo). |
| FR-HOME-3 | SHOULD | P1 | Apply screen-state rules (§5.10) to weather (stale/offline). Time from RTC is always "live". |

**Acceptance:** time is correct and persists across a WiFi outage; weather shows real values for the set location and marks itself stale when fetch fails.

### 5.4 Finance — `FR-FIN` (device-direct)

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-FIN-1 | MUST | P1 | Show a **configurable** list of instruments: FX→IDR (USD/EUR/SGD/JPY/CNY), crypto (BTC), US indices/ETFs (S&P 500, NASDAQ, ARKK), IHSG. |
| FR-FIN-2 | MUST | P1 | Per instrument: current value + signed change (direction shown by sign + glyph + color, never color alone). |
| FR-FIN-3 | MUST | P1 | The instrument list is config-driven (editable without firmware structural change); see `tech.md` for the data sources + cadences. |
| FR-FIN-4 | SHOULD | P1 | Apply screen-state rules; stagger fetches to respect free-API limits. |

**Acceptance:** the configured instruments render with live values + correct up/down indication; removing/adding an instrument in config changes the screen; markets-closed/stale states are honest.

### 5.5 AI Usage — `FR-USAGE` (hub plane)

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-USAGE-1 | MUST | P2 | Show **Claude and Codex, each with BOTH a 5-hour and a 7-day window**: utilization % + reset time (four values total). |
| FR-USAGE-2 | MUST | P2 | Data arrives from the Hub over BLE (the device never holds provider tokens). Refresh ≤ 60s while the Hub is connected. |
| FR-USAGE-3 | MUST | P2 | When the Hub is disconnected/asleep, show last-synced values with age + a "hub offline" indication (§5.10). |

**Acceptance:** all four windows display with reset times; values track the Hub within a minute; disconnecting the Hub flips the screen to last-synced state, not blank or fake. **P2 also re-measures** free internal heap under an *active bonded BLE link + cert-validated TLS + the LVGL UI* and confirms the ≥60 KB floor (`tech.md` §8) — this closes the coexistence proof, which the spike only established for advertising + insecure TLS.

### 5.6 Coding Buddy — `FR-BUDDY` (hub plane)

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-BUDDY-1 | MUST | P2 | **Idle state:** show Claude Code session state pushed by the Hub — running/waiting counts, tokens, context %, recent activity. |
| FR-BUDDY-2 | MUST | P2 | **Prompt state:** when a tool-permission prompt arrives, surface it (tool name + command hint) and let the user **Approve** or **Deny** by tap/gesture; the decision returns to the Hub and resolves the prompt. A prompt the user instead answers on the **Mac** (in the terminal) is **withdrawn from the device silently** — no decision, no "too late". |
| FR-BUDDY-3 | MUST | P2 | Approve/Deny round-trip must complete well within the Claude Code hook timeout (~30s); design for a <5s human action; on timeout the prompt is treated as denied (fail-closed) and labeled. |
| FR-BUDDY-5 | MUST | P2 | The buddy must NOT imply unsupported actions: it **cannot** answer Claude's `AskUserQuestion` multiple-choice prompts (these are passed through to the Mac and surfaced only as a passive "asking a question" indicator — **never** as an Approve/Deny prompt, and never occupying the prompt slot), persist "don't ask again", or type into a live TUI session. |

**Acceptance:** a real Claude Code tool prompt appears on the device and an Approve/Deny tap resolves it in Claude Code; idle state reflects live session counts; unsupported actions are absent from the UI.

### 5.7 Now-Playing — `FR-NOW` (device-direct)

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-NOW-1 | MUST | P4 | Show currently-playing track: title, artist, art (if available), progress, play state, target device. |
| FR-NOW-2 | MUST | P4 | Control playback on an **active Spotify Connect device**: play/pause, next/prev, (volume SHOULD). The device is a remote, not a player; requires Spotify Premium. |
| FR-NOW-3 | MUST | P4 | If no active Connect device exists, say so (can't control a sleeping target); apply screen-state rules. |

**Acceptance:** with Spotify playing on another device, the screen shows the track and transport controls work; with nothing active, it states there's no device to control.

### 5.8 Settings — `FR-SET` (local)

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-SET-1 | MUST | P0 | WiFi provisioning (select/enter network) without reflashing; status shown. |
| FR-SET-2 | MUST | P0 | Brightness control (persisted). |
| FR-SET-3 | MUST | P0 | **Theme picker** (FR-THEME-2). |
| FR-SET-4 | SHOULD | P1 | Edit the finance ticker list and the weather location/timezone. |
| FR-SET-5 | SHOULD | P0 | Idle/sleep timeout; screen enable/reorder. |
| FR-SET-6 | COULD | later | OTA firmware update; "About" (version, device id). |
| FR-SET-7 | SHOULD | P0 | Show battery level + charging state on Settings (color-coded; low battery flagged). |

**Acceptance:** a user can join WiFi, set brightness, pick a theme, and configure tickers from the device; all persist.

### 5.9 macOS Hub companion — `FR-HUB`

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-HUB-1 | MUST | P2 | A macOS menubar app that reads Claude usage (Keychain) + Codex usage (`~/.codex`) and pushes normalized usage to the device over BLE. Tokens never leave the Mac. |
| FR-HUB-2 | MUST | P2 | Ingest Claude Code session/hook events (state + permission prompts) and relay buddy state + prompts to the device; relay Approve/Deny decisions back. |
| FR-HUB-3 | MUST | P2 | Pair/bond with one device securely; reconnect automatically; expose connection status in the menubar. |
| FR-HUB-5 | SHOULD | P2 | Curate the device's finance ticker list from the menubar: search live sources (Binance + Yahoo), add/remove/reorder (≤16), and push the list to the device over BLE as a chunked full-snapshot `config` frame; the device persists it (NVS) and live-applies it without a reboot, acking success/failure (`config_ack`). Validate symbols with a test-fetch before adding. This satisfies the *config-driven ticker list* (FR-FIN-3) via the hub plane; on-device editing (FR-SET-4) remains deferred. (FR-HUB-4 = device-initiated launch, cut — see §7 P2.) |

**Acceptance:** with the hub running, AI Usage + Coding Buddy populate over BLE and approvals work; the device bonds once and **auto-reconnects** after the hub restarts; killing the hub leaves the device in last-synced state. A ticker list curated in the menubar appears live on the device and survives device reboots (NVS).

### 5.10 Cross-cutting: data freshness & states — `FR-STATE`

| ID | Priority | Phase | Requirement |
|---|---|---|---|
| FR-STATE-0 | MUST | **P0** | Define and **freeze the shared contracts** every later phase builds on: the `DataStore` record schema, the `screen_state_t` enum, the transport-agnostic `HubLink` interface, and the config schemas (tickers, weather/time). See `tech.md` §6-§7. |
| FR-STATE-1 | MUST | P1 | Every data screen implements: loading, live, stale (value + age), offline, error/rate-limited, and (hub screens) hub-disconnected. Never render a guessed/stale value as live. |
| FR-STATE-2 | MUST | P1 | Each screen owns a `lastUpdated`; surfaces show age past a per-source threshold (`tech.md`). |
| FR-STATE-3 | MUST | P0 | Network/transport failures never crash or hang the UI; retries use backoff. |

**Acceptance:** pulling WiFi, rate-limiting an API, or quitting the hub each produces the correct visible state, not a frozen or fake screen.

---

## 6. Non-goals (explicit)

On-device Spotify/audio playback · Windows/Linux hub · multi-device/multi-user · cloud sync/accounts · acting as anything other than a remote for Spotify · answering Claude `AskUserQuestion` prompts · storing Claude/Codex secrets on the device.

## 7. Phasing (work breakdown for agents/sessions)

Each phase is independently buildable and reviewable; a session/agent can own one. Order = risk-down, value-up.

| Phase | Scope | Key FRs | Plane | Owner artifact |
|---|---|---|---|---|
| **P0 — Foundation** | boot/power, display+LVGL, theme engine + 7 themes, swipe carousel shells (safe-area), Settings (WiFi/brightness/theme/sleep), NVS, time service (NTP+RTC), **shared contracts** (DataStore, `screen_state_t`, `HubLink` iface, config schemas), lock `SAFE_INSET` + `partitions.csv` | FR-PLAT, FR-THEME, FR-SET-1/2/3/5, FR-STATE-0/3 | device | `firmware/` skeleton + frozen contracts |
| **P1 — Ambient screens** | Home (clock/weather), Finance ticker + config; full screen-state model | FR-HOME, FR-FIN, FR-SET-4, FR-STATE-1/2 | device-direct | data layer + 2 screens |
| **P2 — Hub + AI** | Swift hub app; BLE link (implements `tech.md` §7 protocol); AI Usage; Coding Buddy; **re-measure heap** under active bonded BLE + cert TLS + LVGL | FR-HUB, FR-USAGE, FR-BUDDY | hub plane | `hub/` (Swift) + 2 screens + BLE proto + heap report |
| **P3 — Input polish** | IMU gestures, long-press/quick-settings, motion-wake | FR-PLAT-5/6 | device | input layer |
| **P4 — Now-Playing** | Spotify control + OAuth. **Prereq:** decide OAuth storage (on-device NVS vs proxy, §8) at phase start — not parallel-startable until decided | FR-NOW | device-direct | 1 screen + auth |

**P0 progress (2026-06-07):** done — bring-up (power/display/LVGL/touch, pinned toolchain), `SAFE_INSET`/`partitions.csv` frozen on hardware (FR-PLAT-9), frozen contracts (FR-STATE-0) + theme engine with all 7 themes (FR-THEME-*), and the swipe carousel with six state-aware screens rendered bespoke-per-theme — 42 views (FR-PLAT-2/4, FR-THEME-3/4). Remaining — NVS persistence (FR-PLAT-3), idle dim/sleep (FR-PLAT-7), time service (FR-PLAT-8), WiFi provisioning + live Settings actions (FR-SET-1/2/3/5). The frozen contracts are landed, so P1/P2/P3/P4 are already unblocked.

**P1 progress (2026-06-07):** done and running on hardware — Home (FR-HOME clock+weather via Open-Meteo) and Finance (FR-FIN live FX→IDR/BTC/US indices+ETFs/IHSG, config-driven) over the device-direct WiFi+TLS plane, with the full screen-state model (FR-STATE-1/2: loading/live/stale+age/offline/error). Beyond spec: multi-network WiFi (WiFiMulti) + on-device Wi-Fi manager (incl. on-demand add-network, no reboot), IP-based auto-geolocation. **Deferred:** on-device ticker/location *editing* (FR-SET-4, SHOULD).

**P2 progress (2026-06-08):** code complete and running on hardware. **Device:** `core/hub_proto` Arduino-free codec (+ host tests), `core/hublink_ble` NUS peripheral (bonded LESC, NimBLE-backed core `BLE*` wrapper), `core/hub_task` Core-0 wiring (frame => `ds_set_usage`/`ds_set_buddy`, edge-triggered `ds_set_hub_offline`), 7 buddy decide-paths wired to `hub_send_permission` (send-gated clear), Settings pair overlay. **Hub (`hub/`, SwiftPM menubar app):** `BeaconCentral` (CoreBluetooth NUS central, OS-mediated pairing, acked `.withResponse` RX writes, auto-reconnect), `UsagePoller` (Claude Keychain + Codex `~/.codex`), `ClaudeCodeBridge` (blocking permission hooks + idle/session + statusline shim), `MenubarController`, runs as a signed `.app`. **Live on-device:** AI Usage (Codex via `~/.codex`; Claude via the Claude Code statusline `rate_limits`, with the unofficial `oauth/usage` as an intermittent best-effort fallback) over BLE while the device-direct WiFi/TLS plane runs simultaneously; **heap re-measured** under the full load => LVGL draw buffers moved to PSRAM (`-DBEACON_LVGL_PSRAM`, now the default build flag; `tech.md` §2). **Validated on-device:** buddy permission round-trip (FR-BUDDY-2/3) — approve/deny honored on the Mac. **P2-0 contract capture done (2026-06-11):** §C shapes in `hub/CONTRACT.md` replaced with real token-redacted captures; the P2-0 draft guesses matched the live endpoints, so the normalizer/fixtures now pin to ground truth (#53). **Scope cut:** device-initiated *launch* (FR-BUDDY-4 / FR-HUB-4) removed.

**P2 hub ticker config (2026-06-18, FR-HUB-5):** delivered — the menubar curates the finance ticker list (search Binance `exchangeInfo` cached + Yahoo live, add/remove/reorder ≤16, test-fetch validation) and pushes it over BLE as a chunked full-snapshot `config` frame; the device persists it to NVS (versioned blob, crc32, stable codes) and live-applies it (fetchers + Finance UI rebuild, no reboot), acking per `rev` (`config_ack`, fail-closed). Frozen contract in `hub/CONTRACT.md` §B2 / `tech.md` §7.1. Firmware side also dropped the lagging Frankfurter source (FX now Yahoo `=X`). This realizes the config-driven ticker list (FR-FIN-3) via the hub plane; on-device editing (FR-SET-4) stays deferred.

Dependencies: P1–P4 all depend on **P0 freezing the shared contracts** (FR-STATE-0: `DataStore`, `screen_state_t`, `HubLink`, config schemas) plus `SAFE_INSET` and `partitions.csv`. P2 implements the BLE protocol in `tech.md` §7. Once P0 lands and those contracts are frozen, P1 / P2 / P3 / P4 are parallelizable across separate agents/sessions.

## 8. Open questions / deferred decisions

- **Spotify OAuth**: on-device refresh token in NVS vs a small proxy (Cloudflare Worker). Decide at P4. (`tech.md` notes the security trade-off.)
- **OTA**: in-scope Settings feature vs out-of-band flashing. Since `partitions.csv` is a **P0** deliverable (FR-PLAT-9), the OTA decision is made **in P0**; default = **reserve OTA slots** (changing the layout later reflashes everything). The OTA *update UI* (FR-SET-6) can still come later.
- **Exact panel corner radius**: assumed ~90px; confirm on hardware (cyan-border test) and lock `SAFE_INSET`.
- **BLE stack for the hub link** (resolved at P2): use the Arduino-ESP32 core `BLE*` wrapper, which is **NimBLE-backed** on the pinned esp32s3 libs (Bluedroid is absent for the s3) — verified on hardware. `hublink_ble` calls only the stack-agnostic wrapper (no raw `esp_ble_*`), and the separate `NimBLE-Arduino` (h2zero) 1.4.x library must NOT be added (crashes on core 3.x). See `tech.md` §5/§13.

## 9. Definition of done (per phase)

A phase is done when: its MUST requirements pass their acceptance criteria on real hardware; screen-state rules hold for its screens; it builds with the pinned toolchain (`tech.md`); no secret is committed; and its slice of `README` roadmap is checked off. Design must match `DESIGN.md` tokens/safe-area; code must match `tech.md` conventions.
