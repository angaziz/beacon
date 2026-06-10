# About Panel — Design Spec

Issue: [#52](https://github.com/angaziz/beacon/issues/52) — Settings "About" row is a dead affordance.
Date: 2026-06-10
Labels: enhancement, P1, firmware

## Problem

The Settings "About" row renders a `>` affordance but has a `NULL` tap handler in all 7
theme views. Tapping does nothing — a tappable indicator that does nothing erodes trust.

There is also no firmware version constant anywhere in the codebase, no build-date define,
and no MAC accessor. These must be added.

## Goal

Tapping "About" in every theme opens a minimal, read-only modal showing real device
identity/build info. Shake or tap dismisses it. Firmware builds; host tests pass.

## Decisions (locked)

| Topic | Decision |
|-------|----------|
| Version injection | PlatformIO pre-build script (`extra_scripts = pre:version.py`), reads `FIRMWARE_VERSION` env (CI sets from git tag) else `dev` |
| Build date | Compiler `__DATE__` (compile time == CI build time); no script logic |
| MAC shown | Both WiFi STA + BLE base MACs |
| Extra fields | Version, build date, WiFi MAC, BLE MAC, uptime, free heap |
| Uptime format | Minute resolution, floor to `0m` under 1 min (`0m` / `3m` / `3h 12m` / `2d 4h`) |
| led `V0.1` label | Fix in-scope: replace hardcoded `"V0.1"` (settings_led.cpp:73) with `FIRMWARE_VERSION` |
| Data freshness | Snapshot at open (not live-ticking) — keeps it minimal |

## Architecture

Six components. The panel is **one shared, themed file** — not 7 bespoke layouts.

### 1. Version injection

- `firmware/version.py` — SCons pre-build script:
  ```python
  Import("env")
  import os
  v = os.environ.get("FIRMWARE_VERSION", "") or "dev"
  env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(v))])
  ```
- `firmware/platformio.ini` `[env:beacon]`: add `extra_scripts = pre:version.py`.
  (`[env:native]` is untouched — the version define is only referenced from UI code,
  which the native env excludes.)
- `.github/workflows/release.yml` firmware Build step: add
  ```yaml
  env:
    FIRMWARE_VERSION: ${{ github.ref_name }}
  ```
  Release is a single `release.yml`, triggered on `v*` tags, so `github.ref_name` is already
  `v0.1.0` and is used verbatim (hub + firmware + flasher ship as one matched version).
- Safety guard: `firmware/src/config/version.h` provides the `#ifndef FIRMWARE_VERSION
  #define ... "dev" #endif` fallback, included by every consumer (`about_panel.cpp`,
  `settings_led.cpp`) — single source rather than per-TU inline guards.

Result: tagged CI build => the git tag (e.g. `v0.1.0`); any other build => `dev`.

### 2. Pure formatters — `firmware/src/core/about_format.{h,cpp}`

Arduino/LVGL-free so they compile and unit-test on the host (`BEACON_NATIVE`).

```cpp
void about_fmt_mac(const uint8_t mac[6], char out[18]);     // "AA:BB:CC:DD:EE:FF"
void about_fmt_uptime(uint32_t secs, char* out, size_t cap);// "0m"/"3m"/"3h 12m"/"2d 4h"
void about_fmt_heap_kb(uint32_t bytes, char* out, size_t cap);// "142 KB"
```

`about_fmt_uptime` rule: `secs < 3600` => `"<min>m"` (0m when under a minute);
`secs < 86400` => `"<h>h <m>m"`; else `"<d>d <h>h"`.

### 3. About panel — `firmware/src/ui/about_panel.{h,cpp}`

Near-clone of `theme_panel.cpp`. Public API mirrors the siblings:

```cpp
void about_panel_open(void);
bool about_panel_is_open(void);
void about_panel_close(void);
```

- Full-screen modal `lv_obj_create(lv_layer_top())`, themed via `theme_active()`,
  `bg` cover, `CLICKABLE` (so taps don't leak to the carousel), `carousel_set_swipe_enabled(false)`
  on open / `true` on close.
- `< back` label top-left (themed `ink_dim`, ext click area 24), `ABOUT` label top-right.
- A `LV_FLEX_FLOW_COLUMN` list of read-only rows; each row = name label (left) + value
  label (right), themed `f_body`/`f_display` + `ink`/`ink_dim` per the theme_panel `mklabel`.
- Rows + data sources:
  | Row | Source | Include |
  |-----|--------|---------|
  | VERSION | `FIRMWARE_VERSION` | (define) |
  | BUILD | `__DATE__` | — |
  | WIFI | `esp_read_mac(m, ESP_MAC_WIFI_STA)` -> `about_fmt_mac` | `esp_mac.h` |
  | BLE | `esp_read_mac(m, ESP_MAC_BT)` -> `about_fmt_mac` | `esp_mac.h` |
  | UPTIME | `millis()/1000` -> `about_fmt_uptime` | `Arduino.h` |
  | HEAP | `esp_get_free_heap_size()` -> `about_fmt_heap_kb` | `esp_system.h` |
- **Dismiss**: `< back` tap, tap anywhere on the root, or shake (via dispatcher).
  Closing from the root's own click event deletes the root mid-dispatch — unsafe — so
  the tap-to-close path defers with `lv_async_call(close_async, NULL)` (same deferral
  rationale as theme_panel's `apply_pick`). `< back` (a child) and the shake path
  (main-loop context) close synchronously.

### 4. Dispatcher — `firmware/src/ui/overlays.cpp`

Add one branch to the existing if/else chain:
```cpp
if (about_panel_is_open()) { about_panel_close(); return true; }
```
Shake already routes through `ui_dismiss_top_overlay()` (main.cpp:134). Order is
immaterial — only one overlay can be open at a time.

### 5. Wire the 7 settings views

Each view: add `#include "ui/about_panel.h"` and
`static void about_cb(lv_event_t*) { about_panel_open(); }`.

| View | Wiring |
|------|--------|
| `settings_editorial.cpp:57` | `row(...,NULL)` => `row(...,about_cb)` |
| `settings_calm.cpp:120` | `mk_row(...,NULL)` => `mk_row(...,about_cb)` |
| `settings_hud.cpp:114` | `make_row(...,NULL)` => `make_row(...,about_cb)` |
| `settings_blueprint.cpp:105` | `make_row(...,false,NULL)` => `make_row(...,false,about_cb)` |
| `settings_oscilloscope.cpp:105` | `make_row(...,false,NULL)` => `make_row(...,false,about_cb)` |
| `settings_analog.cpp:104` | `make_row(...,false,NULL)` => `make_row(...,false,about_cb)` |
| `settings_led.cpp:117` | `make_row` has no cb param; capture returned row and add `LV_OBJ_FLAG_CLICKABLE` + `lv_obj_add_event_cb(row, about_cb, LV_EVENT_CLICKED, NULL)` — exactly how the THEME row (line 102-105) wires |

The "Tickers" row stays display-only (no `>`) — untouched.

### 6. led header version fix

`settings_led.cpp:73`: `lv_label_set_text(ver, "V0.1")` => `lv_label_set_text(ver, FIRMWARE_VERSION)`.

## Testing & Verification

- **Host**: `pio test -e native` — new table-driven `test/test_about_format/test_main.cpp`
  (mac / uptime boundaries / heap) passes; existing suites unaffected. Add
  `+<core/about_format.cpp>` to the native `build_src_filter`.
- **Firmware**: `pio run -e beacon` compiles clean.
- **Manual (on device)**: tap About in each of the 7 themes => panel opens with real
  values; `< back`, tap, and shake each dismiss; carousel swipe restored after close.

### Sample table for `about_fmt_uptime`

| secs | expected |
|------|----------|
| 30 | `0m` |
| 200 | `3m` |
| 11520 | `3h 12m` |
| 187200 | `2d 4h` |

## Out of scope

- Live-ticking uptime/heap.
- A net/MAC HAL abstraction (raw `esp_read_mac` is sufficient and matches `net.cpp`'s
  direct-API style).

## File summary

New: `firmware/version.py`, `firmware/src/core/about_format.{h,cpp}`,
`firmware/src/ui/about_panel.{h,cpp}`, `firmware/test/test_about_format/test_main.cpp`.
Edit: `firmware/platformio.ini`, `.github/workflows/release.yml`,
`firmware/src/ui/overlays.cpp`, 7 `settings_*.cpp` views.
