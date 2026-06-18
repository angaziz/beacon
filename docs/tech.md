# Beacon — Technical Constitution (Non-Functional)

> **What this is:** the north star for *how* Beacon is built — hardware truth, architecture, stack, non-functional requirements, the inter-component contracts, and coding conventions. Every implementation session must conform to this. Numbers come from the spikes (`docs/spikes/`).
>
> **Companion docs:** [`prd.md`](prd.md) (functional *what*) · `DESIGN.md` (visual system + token *values* + screen states + safe area) · `PRODUCT.md` (strategy) · `docs/research/` (full device + API research) · `docs/spikes/` (proven bring-up + coexistence).
>
> **Authority map (resolve conflicts here):** *what/features/phases* → `prd.md`. *visual token values, theme catalog, screen-state visuals, safe-area* → `DESIGN.md`. *how it's built, NFRs, contracts/schemas, conventions, BLE protocol* → this doc. If `docs/research/` or older text conflicts with these three, these win.

---

## 1. Engineering principles (the constitution)

1. **Evidence over assertion.** Risky/unknown → spike on real hardware before spec or build (`docs/spikes/`).
2. **The device is peripheral.** Optimize for glanceability + a fast action. Latency and legibility beat richness.
3. **Honest state, always.** Never render stale/guessed data as live. Every data path has loading/stale/offline/error states.
4. **Fail safe, never hang.** Transport failure degrades the UI gracefully; never blocks the render loop. Permission decisions fail **closed** (deny).
5. **Secrets stay off the device** except a few scoped device-plane tokens (see §9 token-boundary table). Nothing secret in the repo.
6. **Tokens, not forks.** UI = design tokens + a gauge-style selector; a new theme is data.
7. **Black is free.** Pure-black backgrounds (AMOLED off-pixels) — aesthetic + power.
8. **Surgical, conventional code.** Match surrounding style; every line traces to a requirement; comments explain *why*.

## 2. Hardware spec & budgets (verified)

Target: **Waveshare ESP32-S3-Touch-AMOLED-2.16**. Full detail in `docs/research/`; verified facts from `docs/spikes/`.

| Subsystem | Spec / note |
|---|---|
| SoC | ESP32-S3R8, dual LX7 @240MHz, 512KB internal SRAM |
| PSRAM | 8MB Octal (OPI) — **mandatory** |
| Flash | 16MB NOR (QSPI) |
| Display | 2.16" AMOLED, **466×466** working-driver value (advertised as 480×480; the driver uses 466), **CO5300** over QSPI, rounded-square |
| Brightness | DCS command `0x51` (no PWM backlight; no `Display_Brightness` in GFX 1.6.4 — use raw cmd) |
| Touch | CST9220/CST9217 (CST92xx), I2C @0x5A |
| IMU | QMI8658 6-axis, I2C |
| RTC | PCF85063, I2C |
| Audio | ES8311 out + ES7210 mic (separate chips) — unused (out of scope) |
| PMU | **AXP2101**, I2C — init rails before display |
| Pins | QSPI CS12 SCLK38 SDIO 4/5/6/7 RST2 · I2C SDA15 SCL14 · Touch INT11 |
| Buttons | PWR (~1s on / ~5-6s off, via AXP), BOOT, user-IO18. **No RST.** |
| Radio | WiFi 2.4GHz b/g/n only; BLE 5 (LE) only |

**Measured (spike):** display+power idle ≈ **338KB** free internal heap. WiFi + BLE(Bluedroid, **advertising only**) + **insecure** TLS + display ≈ **160KB** min free internal heap, **8.38MB** PSRAM free, 0 crashes, TLS fetch ~1.5s.

**Proof scope / caveat:** coexistence is proven for *BLE advertising + `setInsecure()` TLS + raw-GFX display*. **P2 re-measured under the full worst case** — active bonded BLE (NimBLE) link + cert-validated TLS + the full LVGL UI on hardware: with LVGL draw buffers in **internal SRAM** the min free internal heap collapsed (~44 KB) and TLS fetches timed out; with **`-DBEACON_LVGL_PSRAM`** (draw buffers in PSRAM, now the default build flag, §6) boot free-internal = ~253 KB and steady-state ~115 KB with a transient **min ~53 KB** under an active link + TLS — TLS fetches succeed (real HTTP 2xx). So the architecture is closed **only with LVGL buffers in PSRAM**; the ~53 KB transient sits just under the 60 KB guideline (§8) but is stable in practice — keep the LAN-WebSocket fallback cheap (transport abstraction, §4) and revisit if it tightens.

## 3. Validated bring-up sequence (boot order)

1. `Wire.begin(15,14)`.
2. **AXP2101 first** (XPowersLib): `setDC1Voltage(3300)`, `setALDO1/2/4Voltage(3300)`, `enableALDO1/2/3/4()`. **ALDO2 = DSI_PWR_EN** (critical for 2.16); ALDO3 = display rail.
3. Display: `Arduino_ESP32QSPI(...)` → `Arduino_CO5300(bus, RST2, 0, 466, 466, 0,0,0,0)` → `gfx->begin()`.
4. Panel cmds: `writeC8D8(0x36,0xA0)` · `writeC8D8(0x53,0x20)` · `writeC8D8(0x51,0xFF)`.
5. LVGL (buffers per §6) → WiFi → BLE. Reference: `docs/spikes/`.

## 4. Architecture

- **Device firmware** (`firmware/`): all on-device UI, the device-direct plane (WiFi+TLS to public APIs), the device side of the hub plane (BLE), persistence, theming, input.
- **Hub** (`hub/`, Swift, P2): macOS menubar app; reads Claude/Codex tokens locally, normalizes usage, ingests Claude Code hook/session events, bridges to device over BLE. Holds all provider secrets.
- **External services**: public APIs (finance/weather/Spotify) hit directly by the device.

**Transport abstraction (required):** the device's hub-data interface sits behind a `HubLink` interface so BLE can be swapped for a LAN WebSocket without touching screens. The frame schema (§7) is transport-independent.

## 5. Tech stack & pinned versions

| Layer | Choice | Version |
|---|---|---|
| Firmware SDK | Arduino-ESP32 core (IDF5) | **3.3.x** |
| UI | LVGL | **8.4.0** |
| Display driver | GFX_Library_for_Arduino (`Arduino_CO5300`, `Arduino_ESP32QSPI`) | 1.6.4 |
| Sensors | SensorLib (QMI8658, PCF85063) | 0.3.3 |
| Power | XPowersLib (AXP2101) | 0.2.6 |
| **BLE** | Core **`BLE*` wrapper** (`BLEDevice`/`BLEServer`/`BLECharacteristic`/`BLESecurity`). On the pinned pioarduino esp32s3 libs (55.03.35) this wrapper is backed by **IDF NimBLE** (`CONFIG_BT_NIMBLE_ENABLED`), not Bluedroid — verified at P2. Use ONLY the stack-agnostic wrapper (no raw `esp_ble_*`/`esp_gap_ble_api.h`), so the link works on either backing and `HubLink` keeps screens unaware. **Do not add the separate `NimBLE-Arduino` library (h2zero) — its 1.4.x crashes on core 3.x; that is a different thing from the core's NimBLE backing.** | core |
| Net | `WiFi`, `WiFiClientSecure` (cert-validated), `HTTPClient` | core |
| JSON | ArduinoJson (filters for big payloads) | 7.x |
| Hub | Swift (CoreBluetooth, Keychain, menubar) | macOS 13+ |

Board build settings: **ESP32S3 Dev Module · PSRAM OPI · Flash 16MB · USB CDC On Boot Enabled · CPU 240MHz · Flash QIO 80MHz**, partition per §11. (These silently reset on port re-enumeration — `docs/spikes/SETUP.md`.)

## 6. Firmware architecture

**Task/core model.** Core 0: networking (WiFi/BLE/HTTP) + IMU. Core 1: LVGL render + UI. No I/O blocks the LVGL loop — data tasks publish into the thread-safe `DataStore`; UI reads snapshots. Watchdog enabled.

**Display/LVGL buffers (canonical — supersedes other docs).** Use LVGL **partial render** with **two draw buffers, each ≈ 1/10 screen** (466×47×2 ≈ 44 KB). Allocate in **internal DMA-capable SRAM** *only if* the ≥60 KB free-internal-heap floor (§8) still holds after WiFi+BLE+TLS; otherwise allocate in **PSRAM** (slower but safe). **No full-screen framebuffer.** Decision is made once in the LVGL port module and asserted at boot (log the chosen region). One theme's styles/fonts are resident at a time.

**Theme engine.** `DESIGN.md` is the authority for token **values** and the theme catalog; this doc defines the **runtime contract**:
```c
typedef enum { GAUGE_BAR, GAUGE_RING, GAUGE_CELL, GAUGE_WAVEFORM, GAUGE_MEASURE, GAUGE_BIGFIG, GAUGE_SUBDIAL } gauge_style_t;
typedef struct {
  const char* id;                 // canonical theme id (see DESIGN theme catalog)
  lv_color_t  bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  const lv_font_t *f_hero;        // oversized figures (clock, big %): digit/symbol subset
  const lv_font_t *f_display, *f_body, *f_mono; // display=titles/figures; body/mono per role
  gauge_style_t gauge;
  uint8_t glow;                   // 0..255
  uint8_t radius;                 // corner-radius token
  uint8_t stroke_hair, stroke_med;// line weights
} beacon_theme_t;
```
Switching theme rebuilds the active screen (no reboot). Fonts are flash-resident, glyph-subset to used characters. Screens read tokens only — no hardcoded colors/fonts.

**Screen lifecycle.** Each screen module exposes `build()/update(const Snapshot&)/destroy()`. Only the visible screen is built (neighbors may be kept warm). Screens never fetch — they render from `DataStore` snapshots + a per-screen `screen_state_t` (see below). `SAFE_INSET` (default **40 px**, locked on hardware at P0) bounds all layout.

**DataStore + screen-state (P0 shared contract — every later screen depends on it).**
```c
typedef enum { ST_LOADING, ST_LIVE, ST_STALE, ST_OFFLINE, ST_ERROR, ST_HUB_OFFLINE, ST_RECONNECTING } screen_state_t;
typedef enum { ERR_NONE, ERR_TIMEOUT, ERR_HTTP, ERR_RATE_LIMITED, ERR_PARSE, ERR_NO_ROUTE } data_err_t;
typedef struct { uint32_t last_updated; screen_state_t state; data_err_t err; } record_hdr_t;
```
Each domain has its own typed record (`weather_rec_t`, `finance_rec_t` (array), `usage_rec_t`, `buddy_rec_t`, `nowplaying_rec_t`) embedding `record_hdr_t` as its first member; all string fields are fixed-capacity NUL-terminated buffers (named `*_LEN`), and writers truncate. Fetchers run on timers with backoff and write `{value, last_updated, state}`. Mark `ST_STALE` at the source's `stale_s`; show age once stale. The staleness sweep may only promote `ST_LIVE => ST_STALE` — it never overwrites `ST_OFFLINE`/`ST_ERROR`/`ST_HUB_OFFLINE`. The full record schema + `screen_state_t` are frozen in P0 (`src/core/records.h`, `screen_state.h`) so P1/P2/P4 build against a stable contract.

**Config schemas (NVS + compiled defaults).**
- **Tickers** (`config/tickers.h` defaults): array of
  ```
  { id, source: "binance"|"yahoo", symbol, display_name,
    kind: "fx"|"crypto"|"index"|"etf", cadence_s, stale_s, change_basis: "prev_close"|"24h" }
  ```
  Canonical default set + exact endpoints/cadences live in `docs/research/` §2.3; e.g. S&P 500 = `{source:"yahoo", symbol:"%5EGSPC", display_name:"S&P 500", kind:"index", change_basis:"prev_close"}`.
- **Weather/time** (`config` + NVS): `{ lat, lon, units:"metric", tz_id (IANA), wmo_map (code→label/icon), ntp_server }`. Default Jakarta `lat -6.2, lon 106.8, tz "Asia/Jakarta"`. WMO map is a fixed table in firmware.
- **WiFi (P1, multi-network):** up to `WIFI_MAX_SAVED` networks `{ssid,pass}` in one NVS blob; `WiFiMulti` auto-joins the strongest available. Provisioning = **SoftAP captive portal** (`Beacon-setup` AP, **appends** to the saved list) reached on first boot (empty list), a **touch-hold-at-boot hatch**, or **on demand from the Wi-Fi manager** (**+ add network** — re-opens the portal live, **no reboot**). On-device **Wi-Fi manager** (Settings → tap Wi-Fi): list saved networks, **add network**, forget, Connect/Disconnect toggle. WiFi (dis)connect/scan **and the runtime setup AP (`AP_STA`)** run only on the Core-0 fetch task; the UI mutates the saved list (locked) + sets request flags + reads a published status snapshot — no `WiFi.*` on Core-1. No on-screen keyboard (entering creds is on the portal).

**Networking.** WiFi STA (multi-network via `WiFiMulti` — auto-joins the strongest saved net, see §6 below) + BLE coexist. TLS via `WiFiClientSecure` with a bundled root-CA set + rotation handling; **never `setInsecure()` in product**. One TLS socket at a time, serialized; the Core-0 fetch task is the only caller. Bundled roots (P1, verified against live chains): **ISRG Root X1, DigiCert Global Root G2, GTS Root R1, GlobalSign Root CA, Starfield Services Root CA G2** — covering Open-Meteo / ipwho.is / Binance-data-mirror / Yahoo / BigDataCloud. Cadences:

| Source | Cadence | Stale at |
|---|---|---|
| Time (NTP) | 1/day + RTC offline; resync on connect | n/a (RTC live) |
| Location (IP geo: ipwho.is + BigDataCloud) | once per (re)connect | n/a |
| Weather (Open-Meteo, geo lat/lon) | 10 min | 30 min |
| FX (**Yahoo `<PAIR>=X`** — near-live) | 5 min | 10 min |
| Crypto (**`data-api.binance.vision`** mirror) / indices/ETF (Yahoo) | 1–5 min, staggered | 10 min |
| AI usage (Hub/BLE) | ≤60 s connected | 5 min / hub-offline |
| Spotify now-playing | 1–5 s, only on that screen | 15 s |

## 7. Inter-component contracts

### 7.0 `HubLink` interface (frozen in P0)

Transport-agnostic interface the screens depend on; the BLE link (§7.1) and the LAN-WebSocket fallback both implement it. Freeze this signature in P0 (P2 provides the BLE implementation — NimBLE-backed core `BLE*` wrapper, §5):
```c
typedef void (*hub_frame_cb)(const char* json, size_t len);  // one reassembled status frame
class HubLink {
public:
  virtual bool begin() = 0;                  // start transport (advertise / connect)
  virtual bool isConnected() = 0;
  virtual void onFrame(hub_frame_cb cb) = 0; // hub->device status frames (reassembled, newline-stripped)
  virtual bool send(const char* json, size_t len) = 0; // device->hub command; false if not connected
  virtual void loop() = 0;                    // pumped from a core-0 task
};
```
Screens call `send()` for permission decisions and render from `DataStore` snapshots that the `onFrame` handler updates; on disconnect the store flips usage/buddy records to `ST_HUB_OFFLINE`.

### 7.1 Hub ↔ Device (BLE) — P2 deliverable

Custom GATT over a **Nordic-UART-style service**, bonded + LE-Secure-Connections encrypted. Device = peripheral, advertises a name prefixed `Beacon`. Hub = central.

- Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (central→device, write/no-resp) `6e400002-...`
- TX (device→central, notify) `6e400003-...`
- **Framing:** UTF-8 **newline-delimited JSON**. A frame may span multiple BLE writes/notifies; reassemble on `\n`. Request MTU 247 on connect; do not assume >20 B/packet. The central writes RX with **acknowledged writes** (ATT Write Request / `.withResponse`) — `withoutResponse` packets drop under WiFi+BLE coexistence congestion and corrupt multi-chunk frames (P2 hardware finding).
- **Versioning:** every frame carries `"v":1`. Unknown major version ⇒ ignore + log.
- **Idle vs prompt:** absence of `buddy.prompt` ⇒ idle.

Hub → device (status frame):
```json
{"v":1,"usage":{"claude":{"h5":{"pct":24,"reset":1717600000},"d7":{"pct":24,"reset":1717800000}},
                "codex": {"h5":{"pct":1,"reset":1717590000},"d7":{"pct":29,"reset":1717800000}}},
 "buddy":{"running":2,"waiting":1,"tokens":184502,"context_pct":42,
          "entries":["10:42 git push","10:41 yarn test"],
          "prompt":{"id":"req_abc","tool":"Bash","hint":"rm -rf /tmp/build"}},
 "loc":{"lat":-6.91,"lon":107.61,"tz":"Asia/Jakarta","name":"Sukajadi, Bandung"}}
```
Optional `loc` block (additive `v:1` extension, issue #54): hub-sourced place name from macOS CoreLocation/CLGeocoder + tz from `TimeZone.current`. Independently optional (like `usage`/`buddy`); sent ONLY in the (re)connect full frame and a loc-only frame on meaningful (> ~0.01 deg) change — never on the 30s heartbeat. Device precedence: hub `loc` > cached NVS > IP geolocation; a hub fix is never overwritten by the IP path (`core/location`). Permission denied / no fix => `loc` is omitted and the device keeps the IP-based name.

Device → hub (commands, each acked):
```json
{"v":1,"cmd":"permission","id":"req_abc","decision":"approve"}   // or "deny"
```
Hub → device ack/error:
```json
{"v":1,"ack":"req_abc","ok":true}                 // decision applied
{"v":1,"ack":"req_abc","ok":false}                // decision did NOT apply (late/superseded)
{"v":1,"err":"unknown_prompt_id","id":"req_xyz"}
```
Rules: a `permission` decision MUST echo the originating `prompt.id`; the hub rejects stale/unknown ids (`err`). `ok:false` means the device decided but the hub had already resolved the prompt (e.g. the fail-closed cap fired first, or it was superseded) — the device surfaces this as "did not apply", not success. If Claude Code resolves the prompt on the **Mac** instead (the user answers in the terminal), it closes the held hook connection; the hub **withdraws** the device prompt silently (clears it, frees the slot — no decision frame, no "did not apply"). On disconnect the device shows `ST_HUB_OFFLINE` with last values + age; on reconnect the hub resends a full status frame. `usage` may carry `null` for an unavailable window/provider.

### 7.2 Normalized AI-usage schema (Hub-produced)

The hub normalizes both providers to: `pct` = integer 0–100 (percent of limit used; `null` if unavailable), `reset` = **Unix epoch seconds** (the hub converts Claude's ISO `resets_at` and Codex's epoch to a single unit). Each provider always reports `h5` and `d7`. Raw source shapes + endpoints: `docs/research/` §2.1 (treat as unofficial/unpublished; isolate in the hub).

### 7.3 Claude Code integration (Hub-side) — needed before P2

The hub gets buddy data from Claude Code via documented surfaces (`docs/research/` §2.2):
- **Permission prompts:** `PreToolUse` / `PermissionRequest` **http hooks** → hub; hub forwards to device; device decision returns as the hook's `permissionDecision` (`allow`/`deny`). Blocking ⇒ design for <5 s; on timeout treat as **deny** and label; if the user answers on the Mac (held hook closed), the hub withdraws the device prompt silently (§7.1).
- **Questions (`AskUserQuestion`):** never held as an Approve/Deny prompt — the hub returns `ask` (defers to the Mac's interactive prompt, where the human picks an option) and shows only a passive "asking a question" indicator on the device (§7.1, FR-BUDDY-5).
- **Session/idle state + tokens/context + Claude usage:** statusline JSON (`context_window` + `rate_limits.{five_hour,seven_day}`) + `SessionStart`/`Stop`/`Notification` hook payloads. The hub maps these to the `buddy` block (§7.1) and, since `oauth/usage` now **429s** in practice (Anthropic limits-rule change), reads **Claude usage from the statusline `rate_limits`** (§7.2) — first-party, no token; the oauth endpoint is only a best-effort fallback. The statusline shim wraps the user's existing renderer (forward then delegate), so their status bar is unchanged.
- **Out of scope (FR-BUDDY-5):** the buddy cannot answer `AskUserQuestion` (passed through to the Mac + shown only as a passive indicator, per above), persist "don't ask again", or type into a live TUI.

Exact hook event field names + statusline fields: capture into a `hub/CONTRACT.md` fixture set at P2 start (with recorded sample payloads) so device and hub are tested against the same fixtures.

## 8. Non-functional requirements (targets + how measured)

| Area | Target | How measured |
|---|---|---|
| Frame rate | ≥30 FPS transitions on 466×466 | LVGL `lv_disp` perf monitor / frame timestamps during a scripted swipe |
| Input feedback | <100 ms touch→visual | timestamp touch event → first changed frame |
| Boot | <4 s to first **rendered** screen (WiFi/NTP connect happens async afterward, not counted) | log time from reset to first `lv_scr_load` |
| Memory | LVGL buffers per §6; **≥60 KB free internal heap at all times** | `esp_get_free_internal_heap_size()` min, logged each second under the worst-case screen + active BLE + cert TLS (P2 re-measure) |
| Permission round-trip | <5 s human; hard fail-closed at ~30 s | hook fixture timing |
| Reliability | no crash/hang on WiFi loss, API error, hub disconnect | fault-injection test: drop WiFi, 500/429 from a stub, kill hub — UI stays responsive |
| Power | dark themes; dim→sleep on idle; wake on touch/IMU | (battery current measurement deferred; track sleep entry/exit in logs) |
| Accessibility | body ≥4.5:1, large ≥3:1; targets ≥64 px; reduced-motion alt; no color-only state | contrast check on tokens (`DESIGN.md`); inspect hit-area sizes |
| Observability | `[BEACON]` structured serial logs, level-gated; never log secrets | code review |

## 9. Security & privacy

**Token boundary (authoritative — resolves where each secret may live):**

| Integration | Secret | Lives on | Reaches device? |
|---|---|---|---|
| Claude Code usage | OAuth token (Keychain) | **Hub (Mac) only** | No — only computed `pct`/`reset` over BLE |
| Codex usage | `~/.codex` token | **Hub (Mac) only** | No — only computed `pct`/`reset` over BLE |
| WiFi | SSID/pass | Device NVS | n/a (it's the device's own) |
| Spotify | refresh token | **Decide at P4:** device NVS *or* a proxy (Cloudflare Worker) holding the client secret | Only if NVS path chosen; proxy path keeps it off-device (preferred) |

Rules: Claude/Codex secrets **never** reach the device. Device-plane tokens are **scoped** and prefer a proxy. TLS validates certs (no `setInsecure()` in product). BLE: bonding + allowlist + encrypted chars; per-prompt `id` matching. LAN-WS fallback binds local-only + shared token. Repo: product firmware needs no in-source secrets (WiFi via on-device captive portal + NVS); spikes use editable inline placeholders, with `.gitignore` guarding against committing credential files. Hub logs approve/deny (timestamp + id).

## 10. Coding conventions

**Firmware (C++/Arduino):** modules under `firmware/` (§11), one responsibility each, small files; pins/consts in config headers (no magic numbers in logic). Non-blocking: no I/O `delay()` in the loop; network/sensors on tasks; UI reads snapshots; always handle the error path → a `screen_state_t` (no silent fail). Match existing style; surgical diffs; comments explain *why*; ASCII only (`=>` not `→`). No hardcoded theme colors/fonts in screens; no `setInsecure()`; no secrets in source. Table-driven where it fits (tickers, theme catalog).

**Hub (Swift):** no force-unwraps outside tests; handle Keychain/BLE failures explicitly; isolate token handling; never log token contents; minimal menubar UX with clear connection status.

**Docs:** keep `prd.md`/`tech.md`/`DESIGN.md` current when behavior changes (reflect state, don't append history). Commits are the user's to make.

## 11. Build, flash, test, partitions

Setup/flashing/troubleshooting: **`docs/spikes/SETUP.md`**. Firmware must compile clean against §5 versions and run on hardware before a phase is "done" (`prd.md` §9). Tooling (Arduino IDE vs PlatformIO/arduino-cli) decided at P0 — **PlatformIO recommended** (multi-file, CI-buildable).

**Partition layout is a P0 deliverable** (`partitions.csv`) and must reserve OTA from the start (changing it later reflashes everything). Target 16MB: **two app slots (`ota_0`/`ota_1`, ~3 MB each)** + `nvs` + a `littlefs`/`spiffs` data partition for fonts/assets. Produce a **font/asset budget** (glyph ranges × bpp × sizes per theme) at P0 and confirm it fits the data partition; if OTA is dropped, document it and use a single 3 MB app + larger data partition.

## 12. Repo structure (product firmware)

Spikes stay under `docs/spikes/`. Product firmware lands under a new top-level `firmware/` at P0:
```
firmware/
├── beacon/                 # PlatformIO project (or Arduino sketch)
│   ├── src/
│   │   ├── main.cpp        # setup()/loop() wiring only
│   │   ├── config/         # pins.h, tickers.h, weather defaults, build flags
│   │   ├── hal/            # power(AXP), display(CO5300), touch, imu, rtc
│   │   ├── core/           # datastore, hublink(iface+ble), net(wifi/tls/http), nvs, timekeep
│   │   ├── ui/             # lvgl_port, theme engine + tokens, carousel, components
│   │   └── screens/        # home, finance, usage, buddy, nowplaying, settings
│   └── partitions.csv
hub/                        # macOS Swift app (P2) + CONTRACT.md fixtures
```

## 13. Risks & limitations

- **Coexistence** proven only for advertising + insecure TLS (§2) — **re-measure under active bonded BLE + cert-validated TLS + LVGL at P2.** Keep `HubLink` abstract.
- **Touch is unproven.** Neither spike exercised the CST92xx controller (both are draw-only). Swipe nav (FR-PLAT-2) + wake-on-touch (FR-PLAT-7) are core P0, so touch read/init is the **main unproven-hardware item in P0** — bring it up first; driver via SensorLib / the Waveshare example pack (`docs/research/`).
- **Outlier widgets not started.** The 7 theme fonts are sourced, glyph-subset, budgeted, and on-device-verified (P0-B; `src/ui/fonts/`). The Analog face + Oscilloscope trace remain bespoke widgets (`DESIGN.md`) — currently `gauge.cpp` placeholders (`GAUGE_SUBDIAL`/`GAUGE_WAVEFORM`), deferred past P0.
- **BLE stack:** use the core `BLE*` wrapper only. On the pinned esp32s3 libs it is backed by IDF
  NimBLE (P2 finding; `esp_gap_ble_api.h`/Bluedroid is absent for s3) — the wrapper API is identical
  across backings, so `hublink_ble` calls no raw `esp_ble_*`. The separate `NimBLE-Arduino` (h2zero)
  1.4.x library still crashes on core 3.x — do not add it.
- **Unofficial endpoints:** Claude/Codex usage + Yahoo finance — isolate behind adapters (hub for usage), expect breakage, have fallbacks (`docs/research/`).
- **Spotify:** Premium + active Connect device; control-only; OAuth storage undecided (§9, P4).
- **Panel 466 vs advertised 480**; **corner radius ~90px assumed** — confirm on hardware, lock `SAFE_INSET` at P0.
- **Internal SRAM** is the scarce resource — guard the ≥60 KB floor; buffers per §6; one theme live.

## 14. Glossary

Shared terms (device, hub, plane, theme, window, prompt, stale): `prd.md` §3. Component/contract names (`DataStore`, `HubLink`, `beacon_theme_t`, `screen_state_t`): §6–§7.
