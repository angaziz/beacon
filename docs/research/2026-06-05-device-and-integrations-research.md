# Beacon — Device & Integrations Research

> ESP32-S3 AMOLED 2.16" personal command-center firmware.
> Research date: 2026-06-05. All API endpoints marked "verified" were tested live during research.

## Status — read first (updated 2026-06-05)

The **research findings below remain valid** (hardware specs, API endpoints, prior art, capabilities). The **architecture, SDK recommendation, MVP decomposition, and "Open Decisions" sections are SUPERSEDED** by `PRODUCT.md` + `DESIGN.md` and the locked decisions here. Where this doc conflicts with those, those win.

Locked decisions (override anything below that conflicts):
- **Firmware: Arduino + LVGL v8.4.0 + Bluedroid BLE** (not ESP-IDF / LVGL v9 as Section 3 suggests). Note: NimBLE-Arduino 1.4.x crashes on core 3.x — the spike settled on Bluedroid; see `docs/spikes/`.
- **Two planes:** (a) device-direct over WiFi+TLS = finance, weather, NTP, Spotify, Hermes; (b) Mac-dependent over BLE via a Swift macOS menubar hub = Claude/Codex usage + coding buddy.
- **Secrets:** Claude/Codex tokens live ONLY in the Mac hub, never on the device. Device-plane integrations (Spotify, Hermes) hold their own scoped tokens — prefer a proxy for the Spotify secret.
- **MVP = 6 screens:** Home, Finance, AI Usage, Coding Buddy, Now-Playing (Spotify control), Settings (not the read-only-first decomposition in Section 4).
- **Coding buddy:** monitor state + approve/deny permission prompts (Claude Code `PreToolUse`/`PermissionRequest` hooks, ~30s blocking) + launch `claude -p`. Cannot answer `AskUserQuestion` prompts; cannot type into a live TUI.
- **Spotify:** control only — device is a remote and needs an active Connect device; on-device playback ruled out.
- **AI Usage:** BOTH Claude and Codex expose BOTH a 5-hour AND a 7-day window; the UI shows all four.
- **UI:** 7 selectable themes, default Editorial; theme = design tokens + gauge-style enum.
- **Top risk:** simultaneous WiFi + BLE coexistence on the ESP32-S3; fallback = run buddy/usage over LAN WebSocket (WiFi-only).
- **TLS:** validate certificates (bundle roots, handle rotation); do NOT use `setInsecure()`.
- The usage endpoints in 2.1 are **verified locally but unofficial/unpublished** — isolate them in the hub and expect them to change.

## BLUF

The device is capable of everything requested. The cleanest architecture is **a single host-side daemon (on your VPS or Mac) that aggregates private data (Claude/Codex usage, optionally Hermes) and exposes one small authenticated JSON endpoint, plus the ESP32 polling public keyless APIs directly for finance/weather.** UI on **ESP-IDF + LVGL v9** with a pure-black HUD theme (AMOLED makes black free and beautiful). Two prior projects de-risk the build: **Clawdmeter** (same board, Claude-usage dashboard) and **claude-desktop-buddy-esp32** (the coding buddy, BLE protocol fully documented).

The single biggest architecture fork is the **coding-assistant transport** (BLE + Claude desktop app vs. network bridge + Claude Code hooks) — see Open Decisions.

---

## 1. The Hardware: Waveshare ESP32-S3-Touch-AMOLED-2.16

**Verdict: well-specced for this app. PSRAM + dual codecs + IMU + mic + speaker make every requested feature (incl. voice) physically possible.**

| Subsystem | Detail |
|---|---|
| **SoC** | ESP32-S3R8, dual-core LX7 @240MHz, 512KB SRAM |
| **PSRAM** | 8MB Octal (OPI) — mandatory for framebuffer |
| **Flash** | 16MB NOR (QSPI) |
| **Display** | 2.16" AMOLED, **480×480 advertised** (the working Arduino driver uses **466×466** — confirmed in the spike; design to 466), driver **CO5300**, **QSPI**, 16.7M color, 600 cd/m² |
| **Brightness** | CO5300 register command (no PWM backlight — it's AMOLED) |
| **Touch** | CST9220/CST9217 (CST92xx family), I2C @0x5A, multi-touch + gesture capable |
| **IMU** | **QMI8658** 6-axis (accel+gyro), I2C — enables tilt/shake/flick/tap gestures |
| **RTC** | PCF85063, I2C |
| **Mic** | Dual-mic array + HW echo-cancel, **ES7210** ADC (I2S) — good for wake-word |
| **Audio out** | **ES8311** codec (I2S) + amp + speaker |
| **PMU** | **AXP2101** (I2C) — charging, battery mgmt, power rails, PWR button |
| **Battery** | 3.7V Li-ion via MX1.25 header (SKU 33969 includes battery; 33970/-EN does not) |
| **Buttons** | PWR (active-HIGH via inverter, through AXP2101), BOOT, user button GPIO18 |
| **Storage** | microSD (TF) slot |
| **USB** | USB-C, native ESP32-S3 USB (program + serial) |
| **WiFi** | 2.4GHz 802.11 b/g/n only (no 5GHz) |
| **Bluetooth** | BLE only (BT5 LE) — no Classic |

### Hardware gotchas
- **Init order**: bring up AXP2101 power rails *before* the display, or the panel stays dark.
- **Framebuffer**: a full 466×466 RGB565 frame ≈ 434 KB. Use LVGL partial render (no full framebuffer); buffer placement per `tech.md` §6. QSPI bandwidth is the FPS ceiling — push dirty regions, not full frames.
- **PWR button** is active-HIGH (inverted) and routed through the AXP2101 PMU — handle via PMU, not raw GPIO; long-hold powers off.
- **Two codecs**: ES8311 (out) and ES7210 (in) are separate chips — don't assume one does both.
- **Few free GPIOs** after QSPI display + I2S audio + I2C + SD + buttons. Budget pins carefully.
- **Don't confuse** with `ESP32-C6-Touch-AMOLED-2.16` (same panel, different MCU, no PSRAM). We want the **S3**.
- Exact board dimensions (mm) are only in the wiki's mechanical PDF — confirm before any case/mount work.

### SDK & official examples
- Both **Arduino** (Arduino-ESP32 3.x) and **ESP-IDF** (v5.x) officially supported.
- Official example repo: `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16`.
- Arduino stack: GFX_Library_for_Arduino v1.6.4 + **LVGL v8.4.0** + SensorLib (QMI8658/PCF85063) + XPowersLib (AXP2101).
- ESP-IDF stack: **LVGL v9** + esp-brookesia. ESP-IDF examples include a 64-bar audio spectrum analyzer and an IMU-driven physics demo.
- Ships from factory with a Xiaozhi-style AI assistant demo (we wipe it).
- Flash settings (Arduino): 16MB flash, PSRAM = OPI, USB CDC On Boot = enabled.

Sources: `docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16`, `waveshare.com/esp32-s3-touch-amoled-2.16.htm`, `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16`.

---

## 2. Feature Research

### 2.1 Claude Code + Codex usage limits — SOLVED, verified live

**Both providers expose an OAuth usage endpoint (verified locally; unofficial/unpublished) returning percent-used + reset time for the exact rolling windows. Verified working with your local credentials.**

**Claude Code**
- `GET https://api.anthropic.com/api/oauth/usage`
- Headers: `Authorization: Bearer <token>`, `anthropic-beta: oauth-2025-04-20`
- Token: macOS Keychain service `Claude Code-credentials` (`security find-generic-password -s "Claude Code-credentials" -w`); on Linux `~/.claude/.credentials.json`. Requires `user:profile` scope (your token has it).
- Live response shape:
  ```
  five_hour:  {utilization: 7.0,  resets_at: 2026-06-05T14:20Z}   # 5h session window
  seven_day:  {utilization: 24.0, resets_at: 2026-06-07T23:59Z}   # weekly window
  seven_day_sonnet: {utilization: 2.0, ...}
  ```
  `utilization` is already percent-of-limit.

**OpenAI Codex**
- `GET https://chatgpt.com/backend-api/wham/usage`
- Headers: `Authorization: Bearer <token>`, `chatgpt-account-id: <id>`
- Token: `~/.codex/auth.json` → `tokens.access_token` + `tokens.account_id`. Plan in `id_token` JWT claim.
- Live response: `rate_limit.primary_window` (5h, `used_percent`, `reset_at` epoch), `rate_limit.secondary_window` (7d). Also cached locally in `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl` (`token_count` events, `rate_limits` field) — no network needed for Codex.

**Prior art**
- **Clawdmeter** (`github.com/HermannBjorgvin/Clawdmeter`) — *an ESP32 desk dashboard for Claude usage on a Waveshare ESP32-S3 AMOLED board.* Host daemon reads the Keychain OAuth token, polls every 60s, pushes to device over **BLE**. **This is the closest existing project to ours — study first.**
- **CodexBar** (`github.com/steipete/CodexBar`, 14k★) — macOS menubar doing BOTH Codex and Claude by reusing local CLI OAuth tokens. Windows/Linux ports exist.
- **ccusage** (`github.com/ryoppippi/ccusage`) — pure local log parser of `~/.claude/projects/**/*.jsonl` token counts. Estimates cost, NOT official limit %. Use OAuth endpoints for accurate "% used".

**Recommended pipeline**
```
[host daemon] every 30-60s:
   Claude: keychain token -> GET api.anthropic.com/api/oauth/usage
   Codex:  ~/.codex/auth.json -> GET chatgpt.com/backend-api/wham/usage
   normalize -> {claude:{h5:{pct,reset}, d7:{pct,reset}}, codex:{...}}
   serve via tiny HTTP GET /usage (bearer-protected)
[ESP32] polls GET /usage every 30-60s -> render bars + "resets in Y"
```
Tokens NEVER reach the ESP32 — daemon holds them, serves only percentages. HTTP-poll suits a VPS daemon; BLE-push (Clawdmeter) suits a daemon on a nearby Mac.

### 2.2 Coding assistant buddy — TWO transport options

**Prior art: `claude-desktop-buddy-esp32`** (vthinkxie) — port of Anthropic's official `claude-desktop-buddy` to Waveshare AMOLED boards (incl. our S3-2.16). A desk "pet" visualizing Claude session state + approving/denying tool-permission prompts.

**Key finding — it is BLE-only, driven by the Claude DESKTOP APP (GUI), not Claude Code CLI.** No daemon, no hooks. Protocol fully documented (so we can replicate without their firmware):
- Transport: **Nordic UART Service (NUS)**, newline-delimited JSON, LE Secure bonding w/ 6-digit passkey.
  - Service `6e400001-...`, RX(write) `...0002`, TX(notify) `...0003`. Device name starts with `Claude`.
- Desktop→device status frame:
  ```json
  {"total":3,"running":1,"waiting":1,"msg":"approve: Bash",
   "entries":["10:42 git push","10:41 yarn test"],
   "tokens":184502,"tokens_today":31200,
   "prompt":{"id":"req_abc123","tool":"Bash","hint":"rm -rf /tmp/foo"}}
  ```
- Device→desktop decision: `{"cmd":"permission","id":"req_abc123","decision":"once|deny"}`.
- Enabled via Claude desktop **Developer Mode → Open Hardware Buddy**.
- Firmware: Arduino 3.x, Arduino_GFX + PSRAM canvas (NOT LVGL). 18 ASCII pet "species" w/ state animations (idle/busy/attention/celebrate).

**Option A — BLE + Claude desktop app**: works out of the box if you run the Claude *desktop* GUI app. No networking. Fastest path. Limitation: requires the desktop app running; doesn't work with headless Claude Code CLI on a server.

**Option B — Network bridge + Claude Code hooks**: for driving the buddy from **Claude Code CLI** (incl. headless/VPS) over WiFi. Claude Code supports **`http` hooks** that POST event JSON to a URL:
- Events: `SessionStart/End`, `UserPromptSubmit`, `PreToolUse/PostToolUse` (busy + approval), `Stop`, `Notification` (matchers: `permission_prompt`, `idle_prompt`).
- `PreToolUse` http hook can RETURN a permission decision (`allow`/`deny`/`ask`) — so a device button could approve/deny. Caveat: hook responses are synchronous within Claude Code's timeout; slow human round-trips need care.
- **Statusline** script is the only surface with live token/context metrics.
- Architecture: Claude Code `http` hooks → small bridge server (normalizes to buddy JSON) → WebSocket → ESP32; approvals flow back.

Sources: `github.com/vthinkxie/claude-desktop-buddy-esp32` (+ REFERENCE.md), `github.com/anthropics/claude-desktop-buddy`, `code.claude.com/docs/en/hooks`.

### 2.3 Live financial rates & weather — SOLVED, keyless, verified live

**All four needed feeds are covered by keyless APIs (tested 2026-06-05).**

| Need | API | Key? | Endpoint | Resp | Notes |
|---|---|---|---|---|---|
| **Forex →IDR** | **Frankfurter** | No | `api.frankfurter.dev/v1/latest?base=USD&symbols=IDR,EUR,SGD,JPY,CNY` | ~110B | Daily ECB. Use `.dev` domain. Invert for EUR/IDR etc. |
| **BTC** | **Binance** | No | `api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT` | ~558B | Realtime, +% change. 1200/min |
| **Indices/ETFs + IHSG** | **Yahoo v8 chart** | No* | `query1.finance.yahoo.com/v8/finance/chart/SPY?interval=1d&range=1d` | ~1.2KB | Works for SPY,QQQ,ARKK,^GSPC,^IXIC,**^JKSE**. Needs User-Agent header. One symbol/call. Unofficial |
| **Weather+humidity** | **Open-Meteo** | No | `api.open-meteo.com/v1/forecast?latitude=-6.2&longitude=106.8&current=temperature_2m,relative_humidity_2m,weather_code` | ~420B | 10k/day. WMO code → icon |

- IHSG confirmed via Yahoo `^JKSE` (returns IDR, regularMarketPrice).
- Yahoo is unofficial/ToS-gray/no SLA. Optional hardening: **Finnhub** free (key, 60/min) for SPY/QQQ/ARKK; but keep Yahoo for `^JKSE` (Finnhub free lacks it).
- Fallbacks: `open.er-api.com` (forex), CoinGecko (BTC). Dead/changed: exchangerate.host (now keyed), Yahoo v7 quote (now Unauthorized), CoinCap v2 (dead).
- **TLS**: bundle the needed roots (DigiCert Global Root G2, ISRG Root X1, GTS Root R1) and validate certificates; handle cert rotation. Do NOT use `setInsecure()` — these carry usage/financial/account data. Parse Yahoo with an ArduinoJson filter to avoid full-doc allocation. Stagger requests (forex/weather daily, BTC/indices every few min).

The list of pairs/tickers should be **config-driven in firmware** as requested.

### 2.4 Date/time/weather screen
- Time: NTP over WiFi + onboard **PCF85063 RTC** for offline persistence. Open-Meteo provides temp + humidity + weather_code (map code → icon/text).

### 2.5 System settings (wifi, brightness, etc.)
- WiFi provisioning: ESP-IDF `wifi_provisioning` (SoftAP/BLE) or a captive portal; store creds in NVS.
- Brightness: CO5300 register command (AMOLED dimming), persisted in NVS.
- Settings to expose: WiFi, brightness, theme/accent, screen rotation/order, units/locale, API ticker config, sleep timeout, about/OTA. Persist in NVS.

### 2.6 Voice UX
- **On-device (offline, free): ESP-SR** — AFE (16kHz mono mic) + **WakeNet** (wake word: "Hi ESP"/"Jarvis"/"Computer"; custom = paid commission) + **MultiNet** (≤200 fixed command words, <500ms, English or Chinese). Reference app: **ESP-Skainet**.
- **Free-form dictation needs a server**: capture via I2S → stream (ESP-ADF) → server Whisper/STT → intent back.
- Realistic split: WakeNet+MultiNet on-device for fixed commands ("next", "pause", "show clock"); add cloud STT only if you want arbitrary phrases.
- **Voice basically requires ESP-IDF** (ESP-SR is not cleanly exposed in Arduino).

### 2.7 Gesture UX
- **Touch (LVGL native)**: `LV_EVENT_GESTURE` (swipe up/down/left/right), single/double click, long-press. Realistic nav: horizontal swipe = next/prev screen, vertical = quick settings, long-press = context menu.
- **IMU (QMI8658)**: raise/flick-to-wake, tilt-to-scroll, shake-to-dismiss, double-tap. Simple threshold detectors at 50-100Hz on a dedicated task → post LVGL events. Libs: `lahavg/QMI8658-Arduino-Library`, espp (ESP-IDF).

### 2.8 Hermes Agent (explore) — NEEDS CONFIRMATION
- **Most likely**: `NousResearch/hermes-agent` (Feb 2026, self-improving CLI/gateway agent, $5-VPS self-host). Matches "installed on VPS".
- Interfaces: CLI (`hermes chat`), **gateway mode** (adapters for Telegram/Discord/Slack/WhatsApp/Signal/Email/**Home Assistant**/**ntfy**/Matrix + cron). It's an MCP *client*, not server. **No native REST/WebSocket device API.**
- ESP32 paths (all indirect): (1) **ntfy** channel — cleanest for a microcontroller, plain HTTP pub/sub; (2) Home Assistant bridge; (3) write a small HTTP MCP server the device hits, add to Hermes config (device = a tool Hermes uses); (4) wrap the CLI in your own HTTP/WS shim.
- **Confirm with user**: is this NousResearch hermes-agent? And direction — device controls Hermes, or Hermes uses device?

### 2.9 Spotify control (explore)
- ESP32 **controls** playback (play/pause/next/prev/volume/now-playing) via Spotify Web API — it does NOT stream audio. **Requires Spotify Premium** + a separate active Connect device.
- OAuth: one-time browser login → refresh token stored in NVS; device exchanges for access tokens thereafter. Token not device-bound.
- Scopes: `user-read-playback-state`, `user-modify-playback-state`. Control endpoints return HTTP 204.
- Libs: `witnessmenow/spotify-api-arduino` (established, auto-refresh), `FinianLandes/SpotifyEsp32` (newer). Or offload OAuth/refresh to a Cloudflare Worker proxy (cleanest for a product).

---

## 3. Recommended Technical Stack

| Layer | Choice | Why |
|---|---|---|
| **SDK** | **ESP-IDF v5.x** | Required for ESP-SR voice + QSPI/PSRAM clock control. (Arduino only if we drop on-device voice.) |
| **UI** | **LVGL v9.2+** via `esp_lvgl_port` | Standard, gestures, animations. Pin v9.2+ (v9.0 had ~37% S3 regression). |
| **Design tool** | **SquareLine Studio** | Fast visual LVGL authoring, exports C. |
| **Display** | QSPI, partial-render draw buffer in internal SRAM, bounce buffer | 30-60 FPS; avoid slow PSRAM framebuffer. |
| **Theme** | Pure-black dark HUD, 1-2 neon accents, arcs/rings | Best look + AMOLED power savings (black = pixels off). |
| **Touch input** | LVGL gesture + click/long-press | Swipe navigation. |
| **Motion input** | QMI8658 threshold detectors | Flick-to-wake, shake-back, tap. |
| **Voice (offline)** | ESP-SR WakeNet + MultiNet | Instant fixed commands. |
| **Voice (free-form)** | ESP-ADF stream → server STT | Optional, later. |
| **Host data** | Small daemon (Go/Python) on VPS or Mac | Aggregates Claude/Codex usage; one bearer-protected JSON endpoint. |
| **Public data** | ESP32 polls keyless APIs directly | Frankfurter, Binance, Yahoo, Open-Meteo. |

### Proposed system architecture
```
                         ┌─────────────────── ESP32-S3 (Beacon) ───────────────────┐
                         │  LVGL v9 dark HUD · swipe/IMU gestures · WakeNet voice    │
   Public internet ──────┤  WiFi client                                              │
   (keyless, direct):    │   ├─ Frankfurter  (forex →IDR)                            │
     Frankfurter         │   ├─ Binance      (BTC)                                   │
     Binance             │   ├─ Yahoo v8     (S&P/NASDAQ/ARKK/IHSG)                  │
     Yahoo, Open-Meteo   │   ├─ Open-Meteo   (weather/humidity) + NTP→PCF85063 RTC   │
                         │   └─ GET /usage   (bearer)  ◄──────────┐                  │
                         │   coding buddy:  BLE  OR  WebSocket  ◄─┼──┐               │
                         └────────────────────────────────────────┼──┼──────────────┘
                                                                   │  │
   ┌──────────── Host daemon (VPS or Mac) ──────────────┐         │  │
   │ reads Claude Keychain + ~/.codex/auth.json tokens   │         │  │
   │ polls Anthropic + OpenAI usage endpoints            │── /usage ┘  │
   │ serves normalized usage JSON (bearer-protected)     │            │
   └─────────────────────────────────────────────────────┘            │
   ┌──────────── Coding buddy source (pick one) ─────────┐            │
   │ A) Claude DESKTOP app  ── BLE NUS ──────────────────┼────────────┘ (BLE)
   │ B) Claude Code http hooks → bridge → WebSocket ─────┘              (WiFi)
```

---

## 4. Suggested Decomposition (this spans ~7 subsystems — too big for one spec)

Each phase is independently shippable/testable. Order = risk-down, value-up.

- **Phase 0 — Device foundation**: boot/power (AXP2101), display+LVGL bring-up, dark HUD theme, screen-navigation shell, WiFi provisioning, NVS settings, brightness, time/NTP/RTC. *The platform everything else plugs into.*
- **Phase 1 — Read-only data screens** (lowest risk, all "poll → render"):
  - 1a. Clock/date/weather/humidity screen.
  - 1b. Finance ticker screen (config-driven).
  - 1c. AI usage screen (needs the host daemon).
- **Phase 2 — Coding assistant buddy**: requires transport decision (BLE vs hooks-bridge). Interactive (approvals).
- **Phase 3 — Input layer polish**: IMU gestures + on-device voice (WakeNet/MultiNet).
- **Phase 4 — Explore**: Spotify control; Hermes integration (via ntfy/MCP).

Each phase → its own spec → plan → implementation cycle.

---

## 5. Open Decisions (need your input before design)

1. **Coding-buddy transport** — Do you run the Claude *desktop GUI app*, or only Claude Code CLI (incl. on the VPS)? Determines BLE-app (Option A, fast) vs network-hooks-bridge (Option B, works headless, more build).
2. **Host daemon location** — VPS or your Mac? (You mentioned a VPS. Mac enables BLE-push like Clawdmeter; VPS means HTTP-poll over WiFi + you handle OAuth token refresh since the CLI may not run there.)
3. **SDK** — ESP-IDF (enables on-device voice, recommended) vs Arduino (simpler, matches buddy repo, but no clean ESP-SR). Voice is the deciding factor.
4. **Voice scope** — offline fixed commands only (free, on-device) vs also free-form dictation (needs server)? Or defer voice to Phase 3?
5. **MVP scope** — Confirm Phase 0 + Phase 1 (foundation + the three read-only screens) as the first shippable target, with buddy/voice/Spotify/Hermes as later phases.
6. **Hermes** — Confirm it's `NousResearch/hermes-agent`, and which direction (device controls Hermes vs Hermes uses device).
7. **Device naming** — repo is "beacon"; use as product name?

---

## 6. Key Risks / Caveats
- **Yahoo Finance** is unofficial — may rate-limit by IP / break. Have Finnhub (keyed) as fallback for US tickers; IHSG only via Yahoo.
- **OAuth token refresh** on a headless VPS: if the real CLIs aren't running there to refresh tokens, the daemon must implement the refresh-token flow itself (both store refresh tokens).
- **Spotify** needs Premium + a separate playback device; ESP32 is a remote only.
- **Custom voice wake-word** is a paid Espressif commission; use a built-in ("Jarvis"/"Computer") to stay free.
- **QSPI/PSRAM FPS**: keep draw buffer in SRAM, partial-render; benchmark early.
- **LVGL v9.0 perf regression** — pin v9.2+.
- Confirm exact board mm from wiki mechanical PDF before any enclosure work.

---

## 7. Primary Sources
- Hardware: `docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16`, `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16`
- Usage: `github.com/HermannBjorgvin/Clawdmeter`, `github.com/steipete/CodexBar`, `github.com/ryoppippi/ccusage`
- Buddy: `github.com/vthinkxie/claude-desktop-buddy-esp32`, `github.com/anthropics/claude-desktop-buddy`, `code.claude.com/docs/en/hooks`
- Data APIs: `api.frankfurter.dev`, `api.binance.com`, `query1.finance.yahoo.com/v8`, `api.open-meteo.com`
- UI/voice: `github.com/lvgl/lvgl`, `github.com/espressif/esp-sr`, `github.com/espressif/esp-skainet`, `esp_lvgl_port` perf docs
- Spotify: `github.com/witnessmenow/spotify-api-arduino`
- Hermes: `github.com/nousresearch/hermes-agent`
