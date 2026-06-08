# P1 — Ambient Screens (Home + Finance) — Implementation Plan

> Branch: `feat/p1-ambient-screens`. Scope confirmed with owner: **full P1**, implementing the
> remaining P0 prerequisites (WiFi, time service, NVS) along the way, since P1 depends on them.
> Authority: `prd.md` §5.3/5.4/5.8/5.10 + §7, `tech.md` §6/§7.0/§9/§10. Stop point: code compiles
> clean (`pio run`) and host tests pass (`pio test -e native`); on-device validation is the human test.

## Status (2026-06-08) — implemented; awaiting on-device acceptance

Built on branch `feat/p1-ambient-screens`. `pio run -e beacon` SUCCESS (flash ~52%); `pio test -e native`
45/45 green. Two review passes (plan + code diff) folded in.

- **Done:** time service (PCF85063 RTC + SNTP + POSIX TZ) and clock/date wired into all 7 Home views;
  `now_s()` unified to one epoch; `net` (cert-validated TLS GET, one-socket mutex), `nvs`, WiFi STA +
  SoftAP provisioning portal; weather + finance fetchers (Open-Meteo / Frankfurter / Binance / Yahoo)
  with a Core-0 scheduler (cadence/retry/offline/staleness); persistence restore+save for last-screen /
  brightness / theme; Settings live Wi-Fi status. `BEACON_DEV=0` activates the real data layer.
- **Deferred (documented, owner to decide post-hardware):** **FR-SET-4 runtime editing of the ticker
  list and weather location/timezone.** It is SHOULD-priority and the riskiest UI (touch unproven, no
  keyboard) and ticker editing needs runtime DataStore re-seeding (touching the frozen init). The list
  is already config-driven in firmware (`config/tickers.h`, FR-FIN-3 satisfied); on-device *editing* is
  best built after the data layer is validated on hardware. Per the 2nd-opinion review's recommendation.
- **Out of scope (P0 leftovers, not P1):** FR-PLAT-7 idle dim/sleep, FR-SET-5 sleep-timeout/reorder,
  FR-SET-7 battery-on-Settings finish.
- **Human test = the next step:** flash + validate on the Waveshare board (WiFi join via portal, NTP/RTC
  time across an outage, live weather + tickers with correct up/down + honest stale/offline). This is the
  `prd.md` §9 hardware acceptance; the README P1 box should be checked only after it passes.

## 0. BLUF

P0 froze the contracts (DataStore, `screen_state_t`, `HubLink`, config schemas) and built the LVGL
shell + 7 themes + 6 bespoke per-theme screens that already render from the DataStore. **Everything
visible today runs on dev-seeded fake data** (`ui/dev_seed.cpp`) and a `millis()`-based clock that is
never wired into the Home views. P1 replaces the seed with a **real device-direct data layer**:

```
            ┌─ core/timekeep ─ NTP (SNTP) + PCF85063 RTC + POSIX TZ ─┐
WiFi (STA) ─┤                                                        ├─> DataStore ─> Home/Finance views
   ▲        ├─ fetch/weather   (Open-Meteo)                          │     (already render state via
   │        ├─ fetch/finance   (Frankfurter / Binance / Yahoo)       │      state_view.h helpers)
 core/nvs ──┴─ core/net (WiFiClientSecure cert-validated + backoff)──┘
   ▲
 provisioning (first-boot SoftAP portal) ── Settings (WiFi / tickers / location)
```

The screens and DataStore do **not** change shape — P1 writes into the frozen setters
(`ds_set_weather`, `ds_set_finance`, `ds_set_state_*`) and the staleness sweep already works. The
only screen-side change is **wiring the clock/date** (a static placeholder today) into the 7 Home views.

## 1. What exists vs. what P1 adds

| Layer | Exists (P0) | P1 adds |
|---|---|---|
| Contracts | `core/records.h`, `screen_state.h`, `datastore.{h,cpp}`, `stale.h` | (none — build against them) |
| Config | `config/tickers.h`, `config/location.h` (compiled defaults, frozen schema) | NVS override load/store |
| Time | `now_s() = millis()/1000` in `screen_common.h` + each view | `core/timekeep` epoch; `now_s()` delegates |
| Net | none | `core/net` (WiFi STA + TLS GET), `core/nvs`, provisioning |
| Data | `ui/dev_seed.cpp` (fake) | `fetch/weather.cpp`, `fetch/finance.cpp`, `core/fetch_task.cpp` scheduler |
| Screens | 7 Home + 7 Finance views render weather/finance + state | clock/date wiring in 7 Home views; Settings edit (FR-SET-4) |

## 1.5 Review-driven revisions (verified against the tree — these supersede conflicting text below)

A 2nd-opinion review surfaced issues I verified in-tree; the fixes are now binding:

1. **`now_s()` must be unified across ALL ~30 views, not just Home.** Confirmed: 30 view `.cpp`
   files define a local `static inline now_s(){ millis()/1000 }` and do **not** include
   `screen_common.h`. If fetchers stamp `last_updated = timekeep_now()` (unix epoch) while any view
   ages records with a millis epoch, `record_age_s()` underflows -> permanent `ST_STALE`. **Fix:**
   delete every per-view local `now_s`, route all views through the single `screen_common.h` delegate,
   and the fetch task calls `ds_tick_staleness(timekeep_now())`. One epoch, everywhere.
2. **Do not mutate `config/location.h`** (`// FROZEN schema (FR-STATE-0)`; `test_config` asserts its
   fields). The IANA->POSIX TZ map lives **only** in `timekeep.cpp` as a small lookup table.
3. **Fetchers hold `ST_LOADING` until `timekeep_has_time()` is true.** Avoids the transition-window bug
   where a value stamped pre-time-sync (epoch ~0) flips to instant `ST_STALE` after NTP/RTC jumps the
   clock to ~1.7e9. RTC (battery-backed) usually makes `has_time()` true within boot, so the hold is brief.
4. **Host tests require Arduino-free parse TUs + native-env wiring.** `[env:native]` currently compiles
   only `core/datastore.cpp` (`build_src_filter` in platformio.ini). Pure logic goes in Arduino-free
   files — `fetch/parse_finance.cpp`, `fetch/parse_weather.cpp`, `core/tz_map.cpp`, `core/change_basis.cpp`
   (only `<ArduinoJson.h>` + `records.h` + libc) — added to `build_src_filter`, with ArduinoJson added
   to the native env `lib_deps`. The network shell (`fetch/*.cpp`, `core/net.cpp`) stays thin and is
   on-device only. `pio test -e native` is the gate (NOT `pio run -e native`, which has no `main()`).
5. **Frankfurter `CHG_PREV_CLOSE`:** fetch yesterday's published rate via the dated endpoint
   `/v1/<date>`; on failure show value LIVE with change `--` (drop the "delta-vs-last-value" fallback —
   it measures the wrong thing).
6. **Scheduler stays simple:** one due-queue, pick the most-overdue due source per scheduler tick
   (single in-flight request through the one TLS socket), flat retry delay on error. Add per-source
   exponential backoff only if real 429s appear (Open-Meteo/Binance limits are generous). 429 ->
   `ERR_RATE_LIMITED`, timeout -> `ERR_TIMEOUT`.
7. **NVS budget:** the `nvs` partition is 20 KB (`partitions.csv`) and `esp_wifi` shares it. Call
   `WiFi.persistent(false)` (we own creds in the `beacon` namespace) and keep keys compact; document the
   budget. Keep all 3 root CAs per tech.md §9 (constitution-mandated; flash cost trivial).
8. **LVGL buffer region is frozen at boot** (`lvgl_port`), before WiFi/TLS exist, so the "fall back to
   PSRAM" note is not a runtime valve. cert-TLS handshake + LVGL fonts may threaten the >=60 KB internal
   floor. **Fix:** add a `BEACON_LVGL_PSRAM` build flag; if the on-device heap log shows the floor
   threatened under WiFi+TLS, build with PSRAM buffers. The real number is a human-test output.
9. **Scope honesty:** FR-SET-4 (editable tickers/location) is SHOULD and the riskiest UI (touch
   unproven, no keyboard) — it is the **last, optional** chunk (P1-E); the data-layer milestone does not
   block on it (read-only WiFi/status lands first). Other unfinished **P0** items
   (FR-PLAT-7 idle dim/sleep, FR-SET-5 sleep timeout/reorder, FR-SET-7 battery on Settings) are
   **out of scope** for this P1 plan — noted so they are not silently dropped.

## 2. Chunks (each independently buildable; build after every chunk)

### P1-A — Time service + clock wiring  (FR-PLAT-8, FR-HOME-1)
- **`core/timekeep.{h,cpp}`** (Core-0 owner, read by Core-1):
  - `timekeep_init()` — start PCF85063 (SensorLib `SensorPCF85063`, I2C @0x51 on the shared `Wire`
    bus already begun by power/touch), seed system time from RTC if RTC valid.
  - `timekeep_set_tz(const char* posix_tz)` — `setenv("TZ", ...)` + `tzset()`.
  - SNTP: `esp_sntp`/`configTzTime(posix_tz, ntp1, ntp2, ntp3)` started after WiFi up (P1-B wires the
    callback). On first successful NTP sync, write system time back into the RTC.
  - `timekeep_now()` -> `time_t` epoch (UTC). `timekeep_localtime(struct tm*)` -> local broken-down.
  - `timekeep_has_time()` -> bool (RTC-valid or NTP-synced) for honest "--:--" until time is known.
  - **IANA -> POSIX TZ:** small table in `config/location.h` (`tz_posix` field or a lookup); default
    `Asia/Jakarta => "WIB-7"`. Only the supported set; unknown id => UTC + log.
- **Unify the clock base:** replace the duplicated `now_s()` (in `ui/screens/screen_common.h` and each
  view's local `now_s()`) so staleness ages use the **same epoch** the fetchers stamp. `now_s()` returns
  `(uint32_t)timekeep_now()` in firmware; host/`BEACON_NATIVE` keeps a stub. Fetchers stamp
  `hdr.last_updated = timekeep_now()`.
- **Wire clock/date into the 7 Home views:** each `home_*.cpp` retains its clock + date labels as
  statics and fills them in `update()` from `timekeep_localtime()` (`%H:%M`, date per the view's style).
  Until `timekeep_has_time()`, keep `--:--`. Time from RTC is always "live" (FR-HOME-3); no state chip.
- **`main.cpp`:** call `timekeep_init()` after `touch_begin()` (shared `Wire`), before LVGL is fine.
- Build: `pio run`. Host test: `test/test_timekeep` (tz mapping, `has_time` logic — pure, no RTC).

### P1-B — Net + NVS foundation  (FR-PLAT-3, FR-STATE-3, FR-SET-2)
- **`core/nvs.{h,cpp}`** — thin `Preferences` wrapper, one namespace `beacon`:
  - typed get/set for: `last_screen`(u8), `brightness`(u8), `theme`(u8), `wifi_ssid`/`wifi_pass`(str),
    location override (`lat`,`lon`,`tz_id`), ticker enable/order override (compact blob or per-id keys).
  - `nvs_load_settings(settings_t*)` / `nvs_save_*`. Defaults come from config headers when unset.
  - Persisted-state wiring: carousel restores `last_screen`; theme engine restores `theme`; display
    restores `brightness`. (Closes FR-PLAT-3 / FR-SET-2 / FR-THEME-2 persistence the README flagged.)
- **`core/net.{h,cpp}`** (Core-0):
  - `net_begin()` — `WiFi.mode(WIFI_STA)`, connect from NVS creds (non-blocking; event-driven).
  - `net_is_up()`; on `GOT_IP` start SNTP (P1-A callback) + signal the fetch scheduler.
  - `net_https_get(host, path, headers[], char* out, size_t cap, int* status)` — one shared
    `WiFiClientSecure` + `HTTPClient`, **cert-validated** against a bundled root-CA set
    (`config/root_ca.h`: ISRG Root X1, DigiCert Global Root G2, GTS Root R1). One TLS socket at a time
    (mutex). Timeouts + returns `data_err_t`-mappable result. **Never `setInsecure()`** (tech.md §9).
  - Backoff helper (exponential, capped) for the scheduler.
- Build: `pio run`. Host test: `test/test_nvs` if logic is host-isolable; otherwise covered on device.

### P1-C — WiFi provisioning (SoftAP captive portal)  (FR-SET-1)
- **`core/provision.{h,cpp}`** — if no NVS creds (or held BOOT/long-press): `WiFi.mode(WIFI_AP)`,
  SSID `Beacon-setup`, a tiny `WebServer` captive portal (DNS catch-all) serving a form (SSID list +
  password). On submit: store to NVS, reboot to STA. No on-screen keyboard (tech.md §6).
- **Settings WiFi UI:** show connection status (SSID / IP / OFFLINE) and a "re-provision" affordance
  in the 7 Settings views' WiFi slot (status text only; the portal is the entry path).
- Build: `pio run`. Human-test on device (portal + join).

### P1-D — Weather + finance fetchers + scheduler  (FR-HOME-2, FR-FIN-1/2/3/4, FR-STATE-1/2/3)
- **`fetch/weather.cpp`** — Open-Meteo `current=temperature_2m,relative_humidity_2m,weather_code`
  for NVS/config lat/lon. ArduinoJson parse -> `weather_rec_t` -> `ds_set_weather`; failure ->
  `ds_set_state_weather(ST_ERROR/ST_OFFLINE, err)`.
- **`fetch/finance.cpp`** — one adapter per `ticker_source_t`, dispatched by `DEFAULT_TICKERS[i].source`:
  - `SRC_FRANKFURTER`: `api.frankfurter.dev/v1/latest?base=<sym>&symbols=IDR` (value = X/IDR). Change
    (`CHG_PREV_CLOSE`) via a second call to `/v1/<prev-business-day>` **or** delta vs last stored value
    if prev unavailable; sign/glyph/color handled by the view (FR-FIN-2). One Frankfurter call can batch
    all FX bases — optional optimization, keep per-ticker first for clarity.
  - `SRC_BINANCE`: `api.binance.com/api/v3/ticker/24hr?symbol=<sym>` -> `lastPrice`,
    `priceChangePercent` (`CHG_24H`).
  - `SRC_YAHOO`: `query1.finance.yahoo.com/v8/finance/chart/<sym>?interval=1d&range=1d` with a
    `User-Agent` header; `chart.result[0].meta.regularMarketPrice` + `previousClose` (`CHG_PREV_CLOSE`).
    **ArduinoJson filter** to avoid full-doc allocation (~1.2KB payload).
  - Each writes `ds_set_finance(idx, &rec)` with `hdr.last_updated = timekeep_now()`; per-slot failure
    sets that slot's state only (others keep their own — the array is independently stateful).
- **`core/fetch_task.cpp`** — single Core-0 task (replaces dev_seed's `stale_task`):
  - per-source next-due timers honoring `cadence_s` (staggered to respect free-API limits, FR-FIN-4);
    weather 10–15 min; SNTP 1/day.
  - calls `ds_tick_staleness(timekeep_now())` ~1/s (keeps the existing sweep semantics).
  - on `!net_is_up()`: mark device-plane records `ST_OFFLINE` (don't fetch); on reconnect, refetch.
  - exponential backoff per source on error; maps HTTP 429 -> `ERR_RATE_LIMITED`, timeout -> `ERR_TIMEOUT`.
- **Retire dev_seed for the device path:** gate behind `BEACON_DEV`; default build flips to real data.
  Keep the long-press state-cycler available under `BEACON_DEV` for state-visual testing.
- Build: `pio run`. Host tests: `test/test_finance_parse` (table-driven: sample JSON per source ->
  expected value/change/err), `test/test_weather_parse`, `test/test_change_basis`.

### P1-E — Settings edit + persistence  (FR-SET-4)
- Edit the finance ticker list (enable/disable/reorder from the compiled set) and weather
  location/timezone; persist to NVS (P1-B). Scope to what the bespoke Settings views can host without an
  on-screen keyboard: toggles/reorder for tickers, a small location preset list + tz from the supported
  table. Free-form text entry deferred (documented) — matches the "no keyboard for v1" stance (tech.md §6).
- Build: `pio run`; full `pio test -e native` green.

## 3. New files / touched files

```
src/core/timekeep.{h,cpp}      new   src/core/net.{h,cpp}        new
src/core/nvs.{h,cpp}           new   src/core/provision.{h,cpp}  new
src/core/fetch_task.{h,cpp}    new   src/config/root_ca.h        new
src/fetch/weather.cpp          new   src/fetch/finance.cpp       new
src/config/location.h          edit (+ posix tz)   src/config/tickers.h  (unchanged schema)
src/ui/screens/screen_common.h edit (now_s -> timekeep)
src/ui/screens/views/home_*.cpp (x7) edit (wire clock/date)
src/ui/screens/views/settings_*.cpp (x7) edit (wifi status + ticker/location edit)
src/main.cpp                   edit (init order)   platformio.ini  edit (lib_deps + BEACON_DEV default)
test/test_timekeep, test_finance_parse, test_weather_parse, test_change_basis  new
```

## 4. Conventions / guard rails (tech.md §10)
- No I/O in the LVGL loop: all fetch/WiFi/SNTP/RTC on the **Core-0** fetch task; UI reads snapshots.
- Honest state: every failure path maps to a `screen_state_t` via the existing setters; never render
  stale as live. Clock from RTC is "live"; weather/finance honor loading/live/stale/offline/error.
- TLS validates certs; **no `setInsecure()`**; no secrets in source (`secrets.h`/NVS, gitignored).
- ASCII only (`=>`), surgical diffs, no hardcoded theme colors/fonts in views, table-driven tests.
- `≥60 KB` free internal heap floor: watch the existing heap log; if WiFi+TLS+LVGL pressures it,
  fall back to PSRAM LVGL buffers (already the conditional in `lvgl_port`).

## 5. Risks / decisions
- **Yahoo unofficial** (ToS-gray, no SLA): isolate behind the source adapter; on failure -> `ST_ERROR`,
  not a crash; fallbacks (`open.er-api.com`, CoinGecko) documented, not built in v1.
- **TZ scope:** ship only the supported IANA set (Jakarta default); a full tz database is out of scope.
- **FX change basis:** prev-close needs a second dated Frankfurter call; if it fails, fall back to
  delta-vs-last-value and label honestly. Acceptable for daily ECB data.
- **Heap under cert-TLS + WiFi + LVGL** is unmeasured (P0 only proved insecure TLS + advertising). P1
  surfaces the real number via the heap log; if the floor is threatened, document + keep PSRAM buffers.
- **Touch-driven Settings editing** is the least-proven UI; keep edits to toggles/selects (no keyboard).

## 6. Done = stop-for-human-test
`pio run` builds clean with the real data layer (BEACON_DEV off), `pio test -e native` is green, and
the diff is ready to flash. On-device acceptance (WiFi join, NTP/RTC time across an outage, live weather
+ tickers with correct up/down + honest stale/offline) is the human test the owner runs.
