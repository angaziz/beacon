# Hub-sourced weather + Wi-Fi-off screen gating

Date: 2026-07-19
Status: implemented

## Problem

Two related asks, both driven by battery:

1. When Wi-Fi is switched off (the existing `CONNECT/DISCONNECT` toggle in the Wi-Fi panel,
   `net_set_enabled()`), screens whose data can only come over Wi-Fi should disappear from the
   carousel rather than sit there showing `OFFLINE` forever.
2. Where the hub can supply the same data over BLE, prefer it — so the device does not raise the
   Wi-Fi radio + TLS at all. Radio-up is the dominant power cost in this design.

## What actually depends on Wi-Fi today

| Screen | Source | Without Wi-Fi |
|---|---|---|
| Home — clock/date | PCF85063 RTC + `loc.tz` from hub | **works** |
| Home — weather | Open-Meteo (device HTTPS) | dead today |
| Finance | Yahoo / Binance (device HTTPS) | dead |
| Usage | hub, BLE | works |
| Buddy | hub, BLE | works |
| Settings | local | works |

So only Finance is genuinely Wi-Fi-only. Home is half-dead, and the weather half is exactly what the
hub can cover.

## Decision 1 — weather source on the hub: Open-Meteo, not WeatherKit

- **Weather.app has no public API.** There is no supported way to read its data.
- **WeatherKit** is the same data source and is a real Swift framework, but it requires a paid Apple
  Developer Program membership and the `com.apple.developer.weatherkit` entitlement, which is only
  issued against a real Team ID. `build-app.sh` ad-hoc signs by default (`codesign --force --sign -`),
  and an ad-hoc signature cannot carry that entitlement — local dev builds would break for everyone.
  WeatherKit also imposes mandatory attribution (Apple Weather logo + link) on every surface showing
  its data, which the 466x466 layouts have no room for.
- **Open-Meteo** is what the device already calls (`fetch/weather.cpp`, `fetch/parse_weather.cpp`).
  No key, no entitlement, no attribution constraint. The hub already holds the coordinates it would
  query with — `LocationProvider` (CoreLocation) is the source of the existing `loc` block, so no new
  macOS permission is needed.

Chosen: **hub fetches Open-Meteo over the Mac's connection and forwards a normalized result.**

## Decision 2 — contract: a standalone additive `weather` frame

Follows the `sessions` precedent (§A): the combined `usage`+`buddy`+`loc` frame already nears
`HUB_FRAME_MAX` (1024 B), so weather goes in its own frame rather than embedded. Old firmware ignores
an unknown frame; no version bump.

```json
{"v":1,"weather":{"temp_c":21.4,"rh":58,"wmo":3,"ts":1721390000}}
```

- `temp_c` — float, celsius. `rh` — integer 0..100 relative humidity. `wmo` — Open-Meteo
  `current.weather_code`, consumed by the existing `WMO_MAP` in `config/location.h`; the device's
  code→label/icon table is unchanged.
- `ts` — epoch seconds of the hub's successful fetch, **not** of the frame. This is what
  `weather_rec_t.hdr.last_updated` is set to, so the existing staleness logic works untouched.
- All four fields required; a malformed or partial block is dropped whole (`hub_parse_weather`
  returns false) and the device keeps its last values — same rule as the other blocks.
- Emitted on (re)connect and on the hub's 10-minute poll. **Not** on the 30 s heartbeat.

The hub polls Open-Meteo on the same 10-minute cadence the device uses (`WEATHER_CADENCE_S`), so this
is not extra upstream load — it moves the existing request from the device to the Mac.

## Decision 3 — device precedence: hub weather wins whenever it is fresh

`hub > device fetch`, mirroring the existing `loc` precedence rule. Concretely: while the hub link is
up and its last weather is younger than ~25 min (2.5 cadences), the device **skips its own Open-Meteo
slot entirely** — even when Wi-Fi is on. That is where the battery saving comes from; gating only on
Wi-Fi-off would leave the radio spinning in the common case (hub connected, Wi-Fi also on).

The device falls back to its own fetch when the hub link is down or its weather has aged out, and
Wi-Fi is enabled. With Wi-Fi off and no hub weather, the record goes `ST_OFFLINE` as today.

## Decision 4 — carousel gates Finance only

`MODULES[]` in `ui/carousel.cpp` becomes a runtime-filtered list instead of a fixed array of 5. When
`net_is_enabled()` is false, Finance is dropped; Home stays, because its clock is RTC-backed and its
weather now arrives over BLE.

Rebuild path is the one `on_theme()` already uses (clear pages, re-attach chrome, rebuild views), so
the dot indicator, wrap-around neighbours and `nvs_set_screen()` persistence all follow. Edge cases:

- If the hidden screen is the current one, land on Home.
- The persisted last-screen index must be treated as an index into the *filtered* list, or a reboot
  with Wi-Fi off can restore an index that no longer exists — clamp on restore.
- `carousel_logical_at()` / `carousel_center_slot()` already take `COUNT` as a parameter, so the
  wrap math needs no change; the existing `test_carousel` suite covers it for any count.

## Out of scope

- Finance over BLE. The hub would need per-ticker quote fetching and a much larger frame; not now.
- A separate "hide screens" user setting. The Wi-Fi toggle is the switch.
- Forecast (multi-day). Only `current` is modeled by `weather_rec_t`.

## Work plan

1. `hub/CONTRACT.md` — document the `weather` frame in §A.
2. Hub: `BeaconHubKit/WeatherNormalizer.swift` (Open-Meteo JSON → normalized struct, host-testable) +
   `beacon-hub/WeatherPoller.swift` (10-min URLSession poll, coords from `LocationProvider`) +
   encode/send in `Protocol.swift` / `BeaconCentral.swift`. Tests in `BeaconHubKit` tests.
3. Firmware: `hub_parse_weather()` in `core/hub_proto.cpp` + `test/test_hub_proto` cases.
4. Firmware: hub-weather precedence in `core/fetch_task.cpp` (skip `SRC_WEATHER` when hub weather is
   fresh) + DataStore write path.
5. Firmware: runtime module filtering in `ui/carousel.cpp` + logical-index persisted screen restore.
6. ~~Firmware: Home views show the weather block as `OFFLINE` when neither source has data.~~ Not
   needed — the home views already route `w.hdr.state` through the shared `sv_placeholder`/`sv_dim`/
   `sv_severe` helpers, so `ST_OFFLINE` renders correctly with no per-view change.

## Implementation notes

- Persisted last-screen is now a **logical** (`ALL_MODULES`) index, not a slot. A slot index would
  point at a different screen once the visible set shrinks. Old stored values remain valid: with all
  five screens visible, slot == logical.
- `carousel_goto_buddy()` resolved buddy by the hard-coded index 3. That breaks the moment Finance is
  filtered out, so it now resolves by module identity.
- `mark_offline()` deliberately does **not** flip weather to `ST_OFFLINE` while hub weather is fresh:
  on a Wi-Fi drop that reading is still live and still arriving over BLE.
- Freshness is a timestamp comparison rather than a link-up flag, so a hub that silently stops sending
  ages out on its own and the device resumes fetching without needing a disconnect event.
