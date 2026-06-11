# Source place name from Mac hub via CoreLocation over BLE (issue #54)

## Problem

The time screen's place name is inaccurate. `fetch/geoip.cpp` resolves lat/lon from
`ipwho.is` (ISP point-of-presence, often wrong district/city), then reverse-geocodes via
`api.bigdatacloud.net` with a fragile `description == "city"` heuristic. Both layers are weak.

## Goal

Source location from the Mac hub. macOS CoreLocation (Apple WiFi positioning, no API key) +
`CLGeocoder` gives tens-of-meters accuracy and high-quality place names; tz comes free via
`TimeZone.current.identifier`. The hub already has a live BLE/GATT channel to the device.

Precedence: **hub CoreLocation => cached NVS => IP geolocation**. A hub-sourced location is
never overwritten by the IP path and never auto-reverts (stationary-device assumption).

## Wire format (additive `v:1` extension, NOT a version bump)

```json
{"v":1,"loc":{"lat":-6.91,"lon":107.61,"tz":"Asia/Jakarta","name":"Sukajadi, Bandung"}}
```

`loc` is an independently-optional block (like `usage`/`buddy`). Sent ONLY in the full frame on
device (re)connect and in a loc-only frame on meaningful change. NEVER on the 30s heartbeat.
`loc` block ~85 B; worst status ~600 B; `HUB_FRAME_MAX = 1024` => fits.

## Firmware

### New module `core/location.{h,cpp}` — canonical location owner

In-RAM cache `{float lat, lon; char tz[40]; char place[48]; uint8_t source;}` guarded by a
`ds_lock_t` (cross-task: fetch task + hub task on Core-0; UI read on Core-1). NVS-backed.

```c
typedef enum { LOC_SRC_NONE = 0, LOC_SRC_IP = 1, LOC_SRC_HUB = 2 } loc_source_t;

void         location_begin(void);                       // load cache from NVS at boot
void         location_place(char* out, size_t cap);      // name accessor; "--" until known (locked)
loc_source_t location_source(void);                      // locked read
// Persist coords (keep >0.01 deg wear threshold), place (only if name string changed), source.
// Does NOT touch the clock; the CALLER applies tz outside the lock (avoids tzset-under-lock race).
void         location_set_from_hub(float lat, float lon, const char* place);
void         location_set_from_ip (float lat, float lon, const char* place);
```

Rationale for caller-applied tz: `timekeep_set_tz()` -> `setenv`/`tzset` is not thread-safe and is
already called unlocked from `fetch_geoip`. Keeping tz application out of the location lock avoids a
new deadlock/lock-coupling surface; the location module stays pure state + NVS.

### `nvs.{h,cpp}`

Add, alongside the existing `lat/lon/tz` keys (UNCHANGED — weather + boot tz restore depend on them):

```c
bool    nvs_get_place(char* out, size_t cap);   // false if unset
void    nvs_set_place(const char* name);        // key "place"
uint8_t nvs_get_loc_src(uint8_t def);           // key "loc_src"
void    nvs_set_loc_src(uint8_t v);
```

`nvs_set_location(lat,lon,tz)` signature stays identical. NVS underlying (IDF) is thread-safe; the
location RAM cache is what the `ds_lock` protects (mirrors the WiFi-blob pattern in `nvs.cpp`).

### `fetch/geoip.cpp` + `geoip.h`

- `fetch_geoip()` early-returns `ERR_NONE` when `location_source() == LOC_SRC_HUB` (skips `ipwho.is`
  + BigDataCloud entirely; hub already supplied coords+tz). NTP stays independent.
- IP success path: replace direct `s_city` + `nvs_set_location` writes with
  `location_set_from_ip(lat, lon, name)`, then `timekeep_set_tz(tz)` as today (unlocked).
- `geoip_city()` becomes a shim: fills a function-local `static char[48]` via `location_place()`
  and returns it (cannot return the retired `s_city`). Update the header comment.

### `core/hub_proto.{h,cpp}`

Extend the parser with an optional `loc` out-block, mirroring usage/buddy:

```c
typedef struct { float lat, lon; char tz[40]; char name[48]; } hub_loc_t;

bool hub_parse_status(const char* json, size_t len,
                      usage_rec_t* usage, bool* had_usage,
                      buddy_rec_t* buddy, bool* had_buddy,
                      hub_loc_t*   loc,   bool* had_loc);   // NEW trailing params
```

A loc-only frame `{"v":1,"loc":{...}}` returns `true` with `had_loc=true`, others false. `name`
truncates to `[48]`, `tz` to `[40]` via the existing `copy_trunc`.

### `core/hub_task.cpp`

`on_frame()`: pass `&loc, &had_loc` to `hub_parse_status`. When `had_loc`, call
`location_set_from_hub(loc.lat, loc.lon, loc.name)` then `timekeep_set_tz(loc.tz)` (outside any
lock). Do NOT add any `!had_usage && !had_buddy` rejection — a loc-only frame is valid and must not
log "bad/ignored frame".

### `ui/screens/views/home_calm.cpp`

Bump the local name buffer from `[40]` to `[48]` to match `place[48]`; keep calling `geoip_city()`.

## Hub (Swift)

### New `LocationProvider` (in `beacon-hub`)

Wraps `CLLocationManager` (created on the main thread; `@MainActor`):

- On launch: `requestWhenInUseAuthorization()`. Drive `requestLocation()` from
  `locationManagerDidChangeAuthorization` once status is authorized (NOT directly at launch).
- On `NSWorkspace.didWakeNotification`: one-shot `requestLocation()` (no continuous monitoring).
- `didUpdateLocations`: if coords moved > ~0.01 deg since the last fix, `CLGeocoder`
  `.reverseGeocodeLocation` (completion hops to `@MainActor`) to build `name`; else reuse cached name.
- `name`: "<subLocality>, <locality>" when both present; `.reducedAccuracy` => locality only.
- `tz = TimeZone.current.identifier`. Denied / no fix / `.reducedAccuracy`-still-fine => emit `Loc`
  unless truly unavailable (denied or error => emit nothing => device keeps IP fallback).
- `onFix(Loc)` callback -> `AppDelegate`.

### `BeaconHubKit/Protocol.swift`

```swift
public struct Loc: Codable, Equatable {
    public var lat: Double; public var lon: Double; public var tz: String; public var name: String
}
// StatusFrame gains: public var loc: Loc?   (JSONEncoder omits nil; sortedKeys still fine)
```

### `beacon-hub/AppDelegate.swift`

- `private var lastFix: Loc?`
- Split the shared sender: `sendFullFrame(includeLocation: Bool)`.
  - `onReady` (device re/connect) => `sendFullFrame(includeLocation: true)` (rides `lastFix`).
  - 30s heartbeat => `sendFullFrame(includeLocation: false)` — the heartbeat-exclusion guarantee.
- `LocationProvider.onFix` => cache `lastFix`; if connected, `sendFrame(StatusFrame(loc: fix))`
  (loc-only). The hub-side > 0.01 deg gate throttles sends (also bounds device NVS churn under
  `.reducedAccuracy`).

### `hub/Info.plist`

Add `NSLocationWhenInUseUsageDescription` (correct key for macOS 13 + `requestWhenInUseAuthorization`;
verified against Apple docs). App is not sandboxed (ad-hoc `codesign --sign -`) => no location
entitlement needed; the plist key + a signed bundle suffice for the TCC prompt.

## Tests

- **Native** `test/test_hub_proto/test_main.cpp` (table-driven): update all 10 existing
  `hub_parse_status` call sites for the new params; add cases — loc present fills lat/lon/tz/name +
  `had_loc`; loc absent => `had_loc=false`; loc-only frame returns `true`; `name`>48 truncates;
  `tz`>40 truncates.
- **Swift** `Tests/BeaconHubKitTests/ProtocolTests.swift`: `StatusFrame(loc:).encoded()` emits `loc`
  and omits `usage`/`buddy`; `StatusFrame(usage:)` omits `loc` (heartbeat exclusion); `Loc` Codable
  round-trip.
- Firmware `location` store precedence (hub never overwritten by IP, survives reboot) verified
  on-device — depends on Arduino `Preferences`, not native-testable.

## Docs

- `docs/tech.md §7.1`: add `loc` to the status-frame example + a rule line (loc on connect/change
  only, never heartbeat; precedence hub>nvs>ip).
- `hub/CONTRACT.md §A`: add the `loc` optional block + example (additive `v:1`, FROZEN-compatible).

## Acceptance criteria (from issue #54)

1. Hub connected + permission granted => time screen shows the CLGeocoder name within one connect cycle.
2. `loc` absent from heartbeat frames; present on (re)connect full frame + on meaningful change only.
3. Hub-sourced location survives reboot (NVS) and is never clobbered by a later IP fetch.
4. Hub absent / permission denied => degrades to the current IP flow.
5. No `CLGeocoder` request when coords unchanged since the last fix.

## Out of scope

- The pre-existing cross-task `setenv`/`tzset` race in `timekeep` (not worsened: tz applied outside
  the location lock). A TTL that reverts hub=>IP after a move (issue says never auto-revert).
