# P0-B Contracts + Theme Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Pure-logic tasks (1,2,3,4,5) also use superpowers:test-driven-development — write the failing Unity test FIRST, watch it fail, then make it pass. Steps use checkbox (`- [ ]`) syntax for tracking. Do NOT run `git commit` — every commit step stages files and asks the USER to run the commit.

**Goal:** Land and FREEZE the P0-B shared contracts (five domain records, `screen_state_t`, `HubLink`, two config schemas, thread-safe `DataStore`) plus the full theme engine (`beacon_theme_t` + `gauge_style_t` + 7-theme catalog with fonts + the five token-driven gauges), proven by host unit tests on a new PlatformIO `native` env and an on-device theme-switch + DataStore smoke demo.

**Architecture:** Pure-logic units (`records.h`, `screen_state.h`, `datastore.{h,cpp}`, the config headers, the theme catalog data) are LVGL/Arduino-free so they compile + table-test on the host under PlatformIO's Unity in a new `[env:native]`. Hardware-coupled code (LVGL style building, font pointers, gauge drawing, the on-device demo) stays in `.cpp` translation units guarded out of native. The `DataStore` abstracts its lock behind a thin wrapper (`ds_lock_t`) so native uses a no-op/`std::mutex` stub while device uses a FreeRTOS mutex; critical sections are pure struct copies (`tech.md` §6).

**Tech Stack:** PlatformIO + pioarduino (Arduino-ESP32 core 3.3.5) for `[env:beacon]`; `platform=native` + Unity for `[env:native]`. LVGL 8.4.0, `LV_COLOR_DEPTH=16` (so `lv_color_t` is a 16-bit RGB565 struct). Fonts generated via `npx lv_font_conv`. Target: Waveshare ESP32-S3-Touch-AMOLED-2.16 (466x466, 8MB OPI PSRAM, 16MB flash, ~3MB `ota_0` app slot).

**Source spec:** `docs/specs/2026-06-06-p0b-contracts-theme-design.md` (THE approved spec — this plan implements exactly it; frozen header code below is copied VERBATIM). Token values: `DESIGN.md`. NFR/contracts/conventions: `tech.md` §6/§7/§10. FR refs: `prd.md` §5 (FR-STATE-0, FR-THEME-1/2/3/4). Toolchain: `firmware/beacon/README.md`.

---

## Conventions (apply to every task)

- **ASCII only** in code + docs (`=>` not arrows; the one allowed non-ASCII is the `°` glyph inside font ranges + WMO labels, which is data, not logic).
- **`[BEACON]` logs** via `util/log.h` macros (`LOGI/LOGW/LOGE`) — never `Serial.print` directly in modules. Native code (no Arduino) logs via `printf`/Unity messages only.
- **No magic numbers in logic** — capacities/limits live in the frozen `#define`s and config headers.
- **Table-driven unit tests** — every Unity logic test iterates a `static const struct {...} cases[]` array.
- **Surgical diffs** — match existing style; do not "improve" adjacent P0-A code.
- **TDD for logic** — failing test first (`pio test -e native` => FAIL), minimal impl, re-run (=> PASS), commit checkpoint.
- **Commits are the USER's** — stage with `git add`, suggest a message, never run `git commit`.
- **Frozen headers are verbatim** — Tasks 1/2/3 copy struct/enum/constant text exactly from the spec; do not paraphrase or reorder fields.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/beacon/platformio.ini` | MODIFY: add `[env:native]` (platform=native, Unity, native build flags) without touching `[env:beacon]` |
| `firmware/beacon/src/core/screen_state.h` | FROZEN: `screen_state_t` + `data_err_t` enums + state-priority rule (spec §3.1) |
| `firmware/beacon/src/core/records.h` | FROZEN: string-length `#define`s, `record_hdr_t`, `record_age_s()`, the 5 domain records (spec §3.2) |
| `firmware/beacon/src/config/tickers.h` | FROZEN schema: `ticker_*` enums, `ticker_cfg_t`, `MAX_TICKERS`, `DEFAULT_TICKERS[]` (spec §3.5) |
| `firmware/beacon/src/config/location.h` | FROZEN schema: `location_cfg_t`, `DEFAULT_LOCATION`, `wmo_entry_t`, `WMO_MAP[]` (spec §3.5) |
| `firmware/beacon/src/core/hublink.h` | FROZEN: `hub_frame_cb` typedef + abstract `HubLink` interface (spec §3.4) |
| `firmware/beacon/src/core/ds_lock.h` | Lock wrapper: FreeRTOS mutex on device, `std::mutex` on native (keeps DataStore host-testable) |
| `firmware/beacon/src/core/datastore.h` | DataStore get/set/sweep API (spec §3.3) |
| `firmware/beacon/src/core/datastore.cpp` | DataStore impl: LVGL/Arduino-free; staleness sweep + state-priority + finance-count contract |
| `firmware/beacon/src/core/stale.h` | Per-source `stale_s` constants (weather/usage/buddy/nowplaying) + finance lookup helper |
| `firmware/beacon/src/ui/theme.h` | `gauge_style_t`, `beacon_theme_t`, global space/motion `#define`s, theme API decls (spec §4.1/§4.2) |
| `firmware/beacon/src/ui/theme_catalog.h` | LVGL-free catalog data surface: ids + colors as host-shimmable `bt_rgb_t`; `THEME_COUNT`, id array |
| `firmware/beacon/src/ui/theme.cpp` | `theme_set()` (frozen teardown order), `theme_active()`, `THEMES[7]`, style building (device-side) |
| `firmware/beacon/src/ui/gauge.h` | `gauge_render()` decl + `gauge_val_t` params |
| `firmware/beacon/src/ui/gauge.cpp` | `gauge_render()`: bar/ring/cell/measure/bigfig full; waveform/subdial deferred stub (device-side) |
| `firmware/beacon/src/ui/fonts/` | generated LVGL C-array font subsets (7 display + body + mono) + `MANIFEST.md` |
| `firmware/beacon/src/ui/demo_screen.{h,cpp}` | throwaway theme-switch demo + DataStore smoke task (device-side) |
| `firmware/beacon/test/test_native/` | native-env Unity table-driven tests (one file per unit) |

---

## Task 0: native test env + Unity smoke test

Prove `pio test -e native` runs before writing any contract logic.

**Files:**
- Modify: `firmware/beacon/platformio.ini`
- Create: `firmware/beacon/test/test_native/test_smoke.cpp`

- [ ] **Step 0.1: Add `[env:native]` to platformio.ini (do NOT touch `[env:beacon]`)**

Append below the existing `[env:beacon]` block. PlatformIO's `native` platform builds host binaries with the system compiler; the bundled Unity framework is selected with `test_framework = unity`. `test_filter` scopes each env to its own test folder so the device env never tries to compile native tests and vice-versa.

```ini

[env:native]
; Host-only env for pure-logic unit tests (records / screen_state / datastore /
; config validation / theme catalog). NO Arduino, NO LVGL — those are stubbed or
; kept out of native translation units. Does not affect [env:beacon] above.
platform = native
test_framework = unity
test_filter = test_native/*
build_flags =
  -DBEACON_NATIVE=1        ; guards Arduino/LVGL-coupled code out of host builds
  -I src
  ; NOTE: do NOT add -std=gnu++17 here — build_flags hit Unity's C sources too and clang
  ; rejects a C++ standard on .c files. Apple clang defaults to C++17 for .cpp (std::mutex OK).
build_src_filter =
  +<core/datastore.cpp>    ; the only product .cpp that is LVGL/Arduino-free
```

`-DBEACON_NATIVE=1` is the single guard symbol; product code uses `#ifndef BEACON_NATIVE` to fence off Arduino/LVGL includes. `build_src_filter` compiles ONLY `datastore.cpp` into the host test binary (theme.cpp/gauge.cpp/demo_screen.cpp pull in LVGL and must never reach native). Also pin `[env:beacon]` to its own folder so it never grabs native tests:

- [ ] **Step 0.2: Scope the device env's tests too**

Add this one line inside the existing `[env:beacon]` block (surgical — append after `monitor_filters`):

```ini
test_filter = test_embedded/*
```

(There is no `test_embedded/` folder yet; this simply prevents `[env:beacon]` from ever picking up `test_native/`. On-device checks in this plan run via `pio run -t upload`, not `pio test`.)

- [ ] **Step 0.3: Write the Unity smoke test (proves the harness runs)**

`firmware/beacon/test/test_native/test_smoke.cpp`:

```cpp
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_harness_runs(void) {
  TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_harness_runs);
  return UNITY_END();
}
```

- [ ] **Step 0.4: Verify (host)** — run:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native
```

Expected: build succeeds, output ends with `test_harness_runs ... PASS` and `1 Tests 0 Failures 0 Ignored ... OK`.

- [ ] **Step 0.5: Verify the device env still builds (regression)** — run:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon
```

Expected: P0-A firmware still links clean (Flash/RAM summary printed). No native artifacts leak into it.

- [ ] **Step 0.6: Commit checkpoint** — stage and ask the USER to commit:

```bash
git add firmware/beacon/platformio.ini firmware/beacon/test/test_native/test_smoke.cpp
```

Suggested message: `test(p0b): add native Unity env + smoke test`

---

## Task 1: FROZEN screen_state.h + records.h

Copy spec §3.1 + §3.2 VERBATIM. Test `record_age_s` (incl. `last_updated==0 => UINT32_MAX`) and that records compile on native.

**Files:**
- Create: `firmware/beacon/src/core/screen_state.h`
- Create: `firmware/beacon/src/core/records.h`
- Create: `firmware/beacon/test/test_native/test_records.cpp`

- [ ] **Step 1.1: Write `screen_state.h` (VERBATIM from spec §3.1)**

```c
#pragma once
#include <stdint.h>

typedef enum {
  ST_LOADING,     // first fetch in flight, no value yet
  ST_LIVE,        // fresh value
  ST_STALE,       // last value older than the source's stale_s
  ST_OFFLINE,     // transport down (WiFi for device-plane)
  ST_ERROR,       // fetch failed / rate-limited (see data_err_t)
  ST_HUB_OFFLINE, // BLE hub disconnected (usage/buddy only)
  ST_RECONNECTING // transport re-establishing; actions stay disabled until confirmed (DESIGN.md screen states)
} screen_state_t;

typedef enum {
  ERR_NONE, ERR_TIMEOUT, ERR_HTTP, ERR_RATE_LIMITED, ERR_PARSE, ERR_NO_ROUTE
} data_err_t;

// State priority (frozen rule). The staleness sweep (datastore) may only promote
// ST_LIVE => ST_STALE. It must NEVER overwrite ST_OFFLINE / ST_ERROR / ST_HUB_OFFLINE,
// which are set explicitly by fetchers/transport and cleared only by a successful update
// (ST_LIVE) or an explicit transition. Display precedence high=>low:
// ST_ERROR / ST_OFFLINE / ST_HUB_OFFLINE > ST_STALE > ST_LIVE > ST_LOADING.
```

- [ ] **Step 1.2: Write `records.h` (VERBATIM from spec §3.2)**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "core/screen_state.h"

#define FIN_ID_LEN     16
#define BUDDY_ID_LEN   24
#define BUDDY_TOOL_LEN 24
#define BUDDY_HINT_LEN 80
#define BUDDY_ENTRY_LEN 40
#define BUDDY_ENTRIES   3
#define NP_TITLE_LEN   64
#define NP_ARTIST_LEN  64
#define NP_DEVICE_LEN  32
#define NP_ART_REF_LEN 160

typedef struct {
  uint32_t       last_updated;  // epoch seconds of last successful update; 0 = never
  screen_state_t state;
  data_err_t     err;           // cause when state == ST_ERROR; else ERR_NONE
} record_hdr_t;

// age in seconds since last successful update; UINT32_MAX if never updated.
static inline uint32_t record_age_s(const record_hdr_t* h, uint32_t now) {
  return h->last_updated ? (now - h->last_updated) : UINT32_MAX;
}

// --- Weather (FR-HOME, device-plane) ---
typedef struct {
  record_hdr_t hdr;
  float    temp_c;
  float    humidity_pct;
  uint16_t wmo_code;            // condition; label/icon via WMO_MAP (location.h)
} weather_rec_t;

// --- Finance (FR-FIN, device-plane) — array; each slot independently stateful ---
typedef struct {
  record_hdr_t hdr;            // per-instrument state/age (one may be stale while others are live)
  char    id[FIN_ID_LEN];      // stable key, matches ticker_cfg_t.id
  double  value;
  double  change;              // signed absolute change
  double  change_pct;          // signed percent
} finance_rec_t;

// --- AI usage (FR-USAGE, hub-plane) — mirrors tech.md §7.1/§7.2 BLE JSON ---
typedef struct {
  int16_t  pct;                // 0..100; -1 = null/unavailable (JSON null)
  uint32_t reset;              // epoch seconds; 0 = unknown
} usage_window_t;
typedef struct { usage_window_t h5, d7; } usage_provider_t;
typedef struct {
  record_hdr_t     hdr;        // ST_HUB_OFFLINE when the hub link drops
  usage_provider_t claude, codex;
} usage_rec_t;

// --- Coding buddy (FR-BUDDY, hub-plane) ---
typedef struct {
  bool present;                // a tool-permission prompt is pending (absence => idle)
  char id[BUDDY_ID_LEN];       // prompt id (echoed back on decide)
  char tool[BUDDY_TOOL_LEN];   // tool name
  char hint[BUDDY_HINT_LEN];   // command hint
} buddy_prompt_t;
typedef struct {
  record_hdr_t  hdr;           // ST_HUB_OFFLINE when the hub link drops
  uint8_t       running, waiting;
  uint32_t      tokens;
  uint8_t       context_pct;
  char          entries[BUDDY_ENTRIES][BUDDY_ENTRY_LEN]; // recent activity lines (newest first)
  uint8_t       entry_count;
  buddy_prompt_t prompt;
} buddy_rec_t;

// --- Now-playing (FR-NOW, device-plane) — frozen now so P4 builds against it ---
typedef struct {
  record_hdr_t hdr;
  bool     has_device;         // an active Spotify Connect device exists
  bool     playing;
  char     title[NP_TITLE_LEN];
  char     artist[NP_ARTIST_LEN];
  char     device[NP_DEVICE_LEN];
  uint32_t progress_ms;
  uint32_t duration_ms;
  // Album art contract (frozen so P4 does not thaw the record): the record carries a REFERENCE,
  // not pixels. has_art=false => no art. art_ref is a stable key (e.g. Spotify art URL or its
  // hash) the UI uses to look up a decoded image from a P4-owned art cache; the fetch/decode/
  // cache mechanism + format are decided in P4, but the record surface is fixed here.
  bool     has_art;
  char     art_ref[NP_ART_REF_LEN];
} nowplaying_rec_t;
```

(`#include <stdbool.h>` is added so the headers compile as C and under the native C++ compiler; `bool`/`true`/`false` are used by the buddy/nowplaying records. Everything between the `#define` block and the last struct is the spec text unchanged.)

- [ ] **Step 1.3: Write the FAILING test FIRST (`test_records.cpp`)**

Run `pio test -e native` after this step and BEFORE Step 1.1/1.2 exist — it must FAIL to compile (headers absent), proving the test drives the code. (If executing top-to-bottom, create this file, comment the headers out, confirm FAIL, then add headers and uncomment.)

```cpp
#include <unity.h>
#include <stdint.h>
#include "core/records.h"

void setUp(void) {}
void tearDown(void) {}

// record_age_s: table incl. the never-updated case.
static void test_record_age_s(void) {
  static const struct {
    const char* name;
    uint32_t    last_updated;
    uint32_t    now;
    uint32_t    expect;
  } cases[] = {
    {"never updated => UINT32_MAX", 0,    1000, UINT32_MAX},
    {"same instant => 0",          1000, 1000, 0},
    {"60s ago",                    940,  1000, 60},
    {"epoch1 now1 => 0",           1,    1,    0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    record_hdr_t h = {0};
    h.last_updated = cases[i].last_updated;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      cases[i].expect, record_age_s(&h, cases[i].now), cases[i].name);
  }
}

// Compile-time contract: frozen capacities + record sizes are fixed.
// static_assert fires at compile if a record field/define is accidentally changed.
static void test_records_compile_contract(void) {
  // string-length defines exist with the frozen values
  TEST_ASSERT_EQUAL_INT(16,  FIN_ID_LEN);
  TEST_ASSERT_EQUAL_INT(24,  BUDDY_ID_LEN);
  TEST_ASSERT_EQUAL_INT(3,   BUDDY_ENTRIES);
  TEST_ASSERT_EQUAL_INT(160, NP_ART_REF_LEN);
  // records are real, default-constructible aggregates
  weather_rec_t    w  = {0};
  finance_rec_t    f  = {0};
  usage_rec_t      u  = {0};
  buddy_rec_t      b  = {0};
  nowplaying_rec_t np = {0};
  (void)w; (void)f; (void)u; (void)b; (void)np;
  // header default state is the zero enum (ST_LOADING / ERR_NONE)
  TEST_ASSERT_EQUAL_INT(ST_LOADING, w.hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_NONE,   w.hdr.err);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_age_s);
  RUN_TEST(test_records_compile_contract);
  return UNITY_END();
}
```

- [ ] **Step 1.4: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: both `test_records.cpp` tests PASS plus the prior smoke test (`3 Tests 0 Failures`).

- [ ] **Step 1.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/core/screen_state.h firmware/beacon/src/core/records.h firmware/beacon/test/test_native/test_records.cpp
```

Suggested message: `feat(p0b): freeze screen_state + records contracts (FR-STATE-0)`

---

## Task 2: config/tickers.h + config/location.h

VERBATIM from spec §3.5. Fill `WMO_MAP[]` (spec leaves it as a comment; populate the fixed table from DESIGN.md weather-condition needs). Table-test: ids unique, count <= MAX_TICKERS, enums in range.

**Files:**
- Create: `firmware/beacon/src/config/tickers.h`
- Create: `firmware/beacon/src/config/location.h`
- Create: `firmware/beacon/test/test_native/test_config.cpp`

- [ ] **Step 2.1: Write `tickers.h` (VERBATIM from spec §3.5)**

```c
#pragma once
#include <stdint.h>

typedef enum { SRC_FRANKFURTER, SRC_BINANCE, SRC_YAHOO } ticker_source_t;
typedef enum { KIND_FX_IDR, KIND_CRYPTO, KIND_INDEX, KIND_ETF } ticker_kind_t;
typedef enum { CHG_PREV_CLOSE, CHG_24H } change_basis_t;

typedef struct {
  const char*     id;           // stable key (NVS/DataStore/config linkage); never reuse
  ticker_source_t source;
  const char*     symbol;       // source-specific symbol (e.g. "%5EGSPC" for Yahoo S&P 500)
  const char*     display_name; // shown on the Finance screen
  ticker_kind_t   kind;
  uint16_t        cadence_s;    // fetch period
  uint32_t        stale_s;      // age after which the slot is ST_STALE
  change_basis_t  change_basis;
} ticker_cfg_t;

#define MAX_TICKERS 16

// EDIT HERE to add/remove/reorder instruments. id must stay unique + stable.
// Fields: id, source, symbol, display_name, kind, cadence_s, stale_s, change_basis
static const ticker_cfg_t DEFAULT_TICKERS[] = {
  {"usd_idr", SRC_FRANKFURTER, "USD",     "USD/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"eur_idr", SRC_FRANKFURTER, "EUR",     "EUR/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"sgd_idr", SRC_FRANKFURTER, "SGD",     "SGD/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"jpy_idr", SRC_FRANKFURTER, "JPY",     "JPY/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"cny_idr", SRC_FRANKFURTER, "CNY",     "CNY/IDR", KIND_FX_IDR, 21600, 86400, CHG_PREV_CLOSE},
  {"btc",     SRC_BINANCE,     "BTCUSDT", "BTC",     KIND_CRYPTO, 60,    600,   CHG_24H},
  {"sp500",   SRC_YAHOO,       "%5EGSPC", "S&P 500", KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"nasdaq",  SRC_YAHOO,       "%5EIXIC", "NASDAQ",  KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
  {"arkk",    SRC_YAHOO,       "ARKK",    "ARKK",    KIND_ETF,    300,   600,   CHG_PREV_CLOSE},
  {"ihsg",    SRC_YAHOO,       "%5EJKSE", "IHSG",    KIND_INDEX,  300,   600,   CHG_PREV_CLOSE},
};

// number of active default tickers (capped at MAX_TICKERS by the datastore count contract).
#define DEFAULT_TICKERS_COUNT (sizeof(DEFAULT_TICKERS) / sizeof(DEFAULT_TICKERS[0]))
```

(`DEFAULT_TICKERS_COUNT` is added — `datastore_init()` and the config test both need the count; it is a derived helper, not a new contract field.)

- [ ] **Step 2.2: Write `location.h` (VERBATIM from spec §3.5; populate `WMO_MAP[]`)**

The spec freezes the schema and leaves `WMO_MAP[]` as a comment. Populate it with the fixed WMO weather-code table (the standard Open-Meteo WMO buckets). `icon` is a glyph/asset id string the Home screen resolves later — kept as a short stable token here.

```c
#pragma once
#include <stdint.h>

typedef struct {
  float       lat, lon;
  const char* units;       // "metric"
  const char* tz_id;       // IANA, e.g. "Asia/Jakarta"
  const char* ntp_server;
} location_cfg_t;

static const location_cfg_t DEFAULT_LOCATION = { -6.2f, 106.8f, "metric", "Asia/Jakarta", "pool.ntp.org" };

typedef struct { uint16_t code; const char* label; const char* icon; } wmo_entry_t; // icon = glyph/asset id

// Fixed WMO weather-code table (Open-Meteo buckets). label = short UI string,
// icon = stable asset id the Home screen resolves. Codes not present fall back to "clear".
static const wmo_entry_t WMO_MAP[] = {
  {0,  "Clear",          "clear"},
  {1,  "Mostly clear",   "clear"},
  {2,  "Partly cloudy",  "partly"},
  {3,  "Overcast",       "cloudy"},
  {45, "Fog",            "fog"},
  {48, "Rime fog",       "fog"},
  {51, "Light drizzle",  "drizzle"},
  {53, "Drizzle",        "drizzle"},
  {55, "Heavy drizzle",  "drizzle"},
  {61, "Light rain",     "rain"},
  {63, "Rain",           "rain"},
  {65, "Heavy rain",     "rain"},
  {71, "Light snow",     "snow"},
  {73, "Snow",           "snow"},
  {75, "Heavy snow",     "snow"},
  {80, "Rain showers",   "showers"},
  {81, "Rain showers",   "showers"},
  {82, "Heavy showers",  "showers"},
  {95, "Thunderstorm",   "storm"},
  {96, "Storm + hail",   "storm"},
  {99, "Storm + hail",   "storm"},
};

#define WMO_MAP_COUNT (sizeof(WMO_MAP) / sizeof(WMO_MAP[0]))
```

- [ ] **Step 2.3: Write the FAILING test FIRST (`test_config.cpp`, table-driven)**

```cpp
#include <unity.h>
#include <string.h>
#include "config/tickers.h"
#include "config/location.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ticker_count_within_max(void) {
  TEST_ASSERT_TRUE_MESSAGE(DEFAULT_TICKERS_COUNT <= MAX_TICKERS,
                           "DEFAULT_TICKERS exceeds MAX_TICKERS");
}

static void test_ticker_ids_unique(void) {
  for (size_t i = 0; i < DEFAULT_TICKERS_COUNT; i++) {
    TEST_ASSERT_NOT_NULL(DEFAULT_TICKERS[i].id);
    for (size_t j = i + 1; j < DEFAULT_TICKERS_COUNT; j++) {
      TEST_ASSERT_FALSE_MESSAGE(strcmp(DEFAULT_TICKERS[i].id, DEFAULT_TICKERS[j].id) == 0,
                                "duplicate ticker id");
    }
  }
}

static void test_ticker_enums_in_range(void) {
  for (size_t i = 0; i < DEFAULT_TICKERS_COUNT; i++) {
    const ticker_cfg_t* t = &DEFAULT_TICKERS[i];
    TEST_ASSERT_TRUE_MESSAGE(t->source <= SRC_YAHOO,      "source out of range");
    TEST_ASSERT_TRUE_MESSAGE(t->kind   <= KIND_ETF,       "kind out of range");
    TEST_ASSERT_TRUE_MESSAGE(t->change_basis <= CHG_24H,  "change_basis out of range");
    TEST_ASSERT_NOT_NULL(t->symbol);
    TEST_ASSERT_NOT_NULL(t->display_name);
    TEST_ASSERT_TRUE_MESSAGE(strlen(t->id) < FIN_ID_LEN, "id wider than FIN_ID_LEN");
  }
}

static void test_wmo_codes_unique(void) {
  for (size_t i = 0; i < WMO_MAP_COUNT; i++) {
    for (size_t j = i + 1; j < WMO_MAP_COUNT; j++) {
      TEST_ASSERT_FALSE_MESSAGE(WMO_MAP[i].code == WMO_MAP[j].code, "duplicate WMO code");
    }
  }
}

static void test_default_location(void) {
  TEST_ASSERT_EQUAL_STRING("Asia/Jakarta", DEFAULT_LOCATION.tz_id);
  TEST_ASSERT_EQUAL_STRING("metric",       DEFAULT_LOCATION.units);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ticker_count_within_max);
  RUN_TEST(test_ticker_ids_unique);
  RUN_TEST(test_ticker_enums_in_range);
  RUN_TEST(test_wmo_codes_unique);
  RUN_TEST(test_default_location);
  return UNITY_END();
}
```

`test_config.cpp` includes `config/tickers.h` for `FIN_ID_LEN`? No — `FIN_ID_LEN` lives in `records.h`. Add `#include "core/records.h"` at the top of the test (the id-width assertion needs it). Make that include explicit.

- [ ] **Step 2.4: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: all config tests PASS.

- [ ] **Step 2.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/config/tickers.h firmware/beacon/src/config/location.h firmware/beacon/test/test_native/test_config.cpp
```

Suggested message: `feat(p0b): freeze ticker + location config schemas`

---

## Task 3: core/hublink.h FROZEN interface

VERBATIM from spec §3.4. It is an abstract interface (no impl until P2). Verify it is implementable via a trivial host mock.

**Files:**
- Create: `firmware/beacon/src/core/hublink.h`
- Create: `firmware/beacon/test/test_native/test_hublink.cpp`

- [ ] **Step 3.1: Write `hublink.h` (VERBATIM from spec §3.4)**

```c
#pragma once
#include <stddef.h>

// One reassembled, newline-stripped status frame. Called from loop() context (the task that
// pumps HubLink::loop()). `json` is valid ONLY for the duration of the callback; copy if retained.
typedef void (*hub_frame_cb)(const char* json, size_t len);

class HubLink {
public:
  virtual ~HubLink() {}
  virtual bool begin() = 0;                 // start transport (advertise / connect)
  virtual bool isConnected() = 0;
  virtual void onFrame(hub_frame_cb cb) = 0;// register the status-frame handler
  // Queue a device->hub command. Returns true if ACCEPTED FOR TRANSPORT (enqueued);
  // false if not connected OR the send queue is full. This is NOT the application-level
  // ack (the hub acks commands separately per tech.md §7.1) — it only reports local enqueue.
  // Buffer lifetime (frozen): the implementation MUST copy `json` before returning; the caller
  // may reuse or free the buffer immediately after send() returns.
  virtual bool send(const char* json, size_t len) = 0;
  virtual void loop() = 0;                   // pumped from a Core-0 task
};
```

- [ ] **Step 3.2: Write the test FIRST — a trivial mock proves the interface is implementable + the callback/send contract holds**

```cpp
#include <unity.h>
#include <string.h>
#include "core/hublink.h"

void setUp(void) {}
void tearDown(void) {}

// Minimal concrete HubLink proving every pure-virtual is overridable + the send/onFrame
// semantics compile. Mirrors what the P2 Bluedroid impl must satisfy.
class FakeHub : public HubLink {
public:
  bool connected = false;
  hub_frame_cb cb = nullptr;
  char last_sent[256] = {0};
  bool begin() override { connected = true; return true; }
  bool isConnected() override { return connected; }
  void onFrame(hub_frame_cb c) override { cb = c; }
  bool send(const char* json, size_t len) override {
    if (!connected || len >= sizeof(last_sent)) return false;  // not connected / queue full
    memcpy(last_sent, json, len);                              // MUST copy (frozen lifetime rule)
    last_sent[len] = '\0';
    return true;
  }
  void loop() override { if (cb) cb("{\"v\":1}", 7); }         // deliver one frame
};

static int g_frames = 0;
static char g_last_frame[64] = {0};
static void on_frame(const char* json, size_t len) {
  g_frames++;
  if (len < sizeof(g_last_frame)) { memcpy(g_last_frame, json, len); g_last_frame[len] = '\0'; }
}

static void test_send_requires_connection(void) {
  FakeHub h;
  TEST_ASSERT_FALSE(h.send("x", 1));   // false before begin() (not connected)
  TEST_ASSERT_TRUE(h.begin());
  TEST_ASSERT_TRUE(h.isConnected());
  TEST_ASSERT_TRUE(h.send("hello", 5));
  TEST_ASSERT_EQUAL_STRING("hello", h.last_sent);  // copied, caller buffer free to reuse
}

static void test_onframe_delivers_one_frame(void) {
  FakeHub h;
  g_frames = 0;
  h.begin();
  h.onFrame(on_frame);
  h.loop();
  TEST_ASSERT_EQUAL_INT(1, g_frames);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1}", g_last_frame);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_send_requires_connection);
  RUN_TEST(test_onframe_delivers_one_frame);
  return UNITY_END();
}
```

- [ ] **Step 3.3: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: both HubLink tests PASS (proves the interface compiles + is implementable on native; no LVGL/Arduino dependency).

- [ ] **Step 3.4: Commit checkpoint** —

```bash
git add firmware/beacon/src/core/hublink.h firmware/beacon/test/test_native/test_hublink.cpp
```

Suggested message: `feat(p0b): freeze HubLink transport interface`

---

## Task 4: core/datastore.{h,cpp} (+ ds_lock.h, stale.h)

The spec §3.3 API. LVGL/Arduino-free so it tests on native. Lock abstracted behind `ds_lock.h` (FreeRTOS on device, `std::mutex` on native). TDD: snapshot equality, finance idx isolation, init seeding, staleness sweep (table), explicit state + recovery (table).

**Files:**
- Create: `firmware/beacon/src/core/stale.h`
- Create: `firmware/beacon/src/core/ds_lock.h`
- Create: `firmware/beacon/src/core/datastore.h`
- Create: `firmware/beacon/src/core/datastore.cpp`
- Create: `firmware/beacon/test/test_native/test_datastore.cpp`

- [ ] **Step 4.1: Write `stale.h` (per-source staleness constants + finance lookup)**

Per-source `stale_s` come from `tech.md` §6 cadence table for the non-finance records; finance is per-ticker (`ticker_cfg_t.stale_s`). No magic numbers in the sweep — they live here.

```c
#pragma once
#include <stdint.h>
#include "config/tickers.h"

// Per-source stale thresholds (seconds), tech.md §6 "Stale at" column.
#define STALE_WEATHER_S    1800u   // 30 min
#define STALE_USAGE_S       300u   // 5 min (hub plane)
#define STALE_BUDDY_S       300u   // 5 min (hub plane)
#define STALE_NOWPLAYING_S   15u   // 15 s

// Finance staleness is per-ticker; defaults come from DEFAULT_TICKERS[idx].stale_s.
static inline uint32_t stale_finance_s(uint8_t idx) {
  return (idx < DEFAULT_TICKERS_COUNT) ? DEFAULT_TICKERS[idx].stale_s : 0u;
}
```

- [ ] **Step 4.2: Write `ds_lock.h` (lock wrapper — FreeRTOS on device, std::mutex on native)**

The DataStore must be host-testable, so the FreeRTOS mutex is abstracted. On native (`BEACON_NATIVE`) it is a `std::mutex`; on device it is a FreeRTOS recursive-free mutex. Critical sections are pure struct copies (`tech.md` §6), so a plain mutex is correct.

```c
#pragma once

#ifdef BEACON_NATIVE
  #include <mutex>
  typedef std::mutex ds_lock_t;
  static inline void ds_lock_init(ds_lock_t*)        {}
  static inline void ds_lock_take(ds_lock_t* m)      { m->lock(); }
  static inline void ds_lock_give(ds_lock_t* m)      { m->unlock(); }
#else
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  typedef SemaphoreHandle_t ds_lock_t;
  static inline void ds_lock_init(ds_lock_t* m)      { *m = xSemaphoreCreateMutex(); }
  static inline void ds_lock_take(ds_lock_t* m)      { xSemaphoreTake(*m, portMAX_DELAY); }
  static inline void ds_lock_give(ds_lock_t* m)      { xSemaphoreGive(*m); }
#endif
```

- [ ] **Step 4.3: Write `datastore.h` (spec §3.3 API + the symmetric explicit setters)**

The spec lists the core API and says "symmetric setters per domain (full list in records/datastore headers)". Provide explicit-state setters for every domain, plus the hub-offline + recovery setters.

```c
#pragma once
#include <stdint.h>
#include "core/records.h"
#include "core/screen_state.h"

void datastore_init(void);

// Setters (Core-0 fetchers). Each sets hdr.state=ST_LIVE, hdr.last_updated=now on success.
void ds_set_weather(const weather_rec_t* r);
void ds_set_finance(uint8_t idx, const finance_rec_t* r);   // idx < MAX_TICKERS
void ds_set_usage(const usage_rec_t* r);
void ds_set_buddy(const buddy_rec_t* r);
void ds_set_nowplaying(const nowplaying_rec_t* r);

// Explicit failure/transport transitions (do not touch the value payload).
void ds_set_state_weather(screen_state_t s, data_err_t e);
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e);
void ds_set_state_usage(screen_state_t s, data_err_t e);
void ds_set_state_buddy(screen_state_t s, data_err_t e);
void ds_set_state_nowplaying(screen_state_t s, data_err_t e);
void ds_set_hub_offline(void);   // flips usage + buddy to ST_HUB_OFFLINE

// Getters (Core-1 UI). Return a by-value snapshot taken under the lock.
weather_rec_t    ds_get_weather(void);
finance_rec_t    ds_get_finance(uint8_t idx);
uint8_t          ds_get_finance_count(void);
usage_rec_t      ds_get_usage(void);
buddy_rec_t      ds_get_buddy(void);
nowplaying_rec_t ds_get_nowplaying(void);

// Staleness sweep (called ~1/s from a Core-0 timer). For each record: if state==ST_LIVE and
// record_age_s(hdr, now) >= stale_s(source), promote to ST_STALE (boundary is inclusive: an
// age exactly equal to stale_s is stale). Never overwrites ST_OFFLINE/ST_ERROR/ST_HUB_OFFLINE.
// stale_s comes from ticker_cfg for finance, per-source constants for the rest.
void ds_tick_staleness(uint32_t now);

// Injected clock for the success setters' last_updated stamp. Device installs an epoch-seconds
// source (RTC/NTP) at boot; native tests install a fake so set/sweep are deterministic.
typedef uint32_t (*ds_now_fn)(void);
void ds_set_clock(ds_now_fn fn);
```

(`ds_set_clock` is added so success setters stamp `last_updated` without coupling the DataStore to Arduino's `time()`/RTC — required to keep the unit LVGL/Arduino-free and the tests deterministic. It is an internal seam, not a frozen-contract change.)

- [ ] **Step 4.4: Write `datastore.cpp` (LVGL/Arduino-free impl)**

```cpp
#include "core/datastore.h"
#include "core/ds_lock.h"
#include "core/stale.h"
#include "config/tickers.h"
#include <string.h>

static ds_lock_t       s_lock;
static weather_rec_t    s_weather;
static finance_rec_t    s_finance[MAX_TICKERS];
static uint8_t          s_finance_count;
static usage_rec_t      s_usage;
static buddy_rec_t      s_buddy;
static nowplaying_rec_t s_nowplaying;

static uint32_t ds_now_zero(void) { return 0; }
static ds_now_fn s_now = ds_now_zero;   // device overrides via ds_set_clock at boot

void ds_set_clock(ds_now_fn fn) { s_now = fn ? fn : ds_now_zero; }

void datastore_init(void) {
  ds_lock_init(&s_lock);
  memset(&s_weather, 0, sizeof(s_weather));
  memset(s_finance, 0, sizeof(s_finance));
  memset(&s_usage, 0, sizeof(s_usage));
  memset(&s_buddy, 0, sizeof(s_buddy));
  memset(&s_nowplaying, 0, sizeof(s_nowplaying));

  // Count contract (frozen): finance_count = number of active default tickers, capped at
  // MAX_TICKERS; seed each slot's id from DEFAULT_TICKERS[idx].id with state = ST_LOADING.
  uint8_t n = (uint8_t)DEFAULT_TICKERS_COUNT;
  if (n > MAX_TICKERS) n = MAX_TICKERS;
  s_finance_count = n;
  for (uint8_t i = 0; i < n; i++) {
    strncpy(s_finance[i].id, DEFAULT_TICKERS[i].id, FIN_ID_LEN - 1);
    s_finance[i].id[FIN_ID_LEN - 1] = '\0';
    s_finance[i].hdr.state = ST_LOADING;
    s_finance[i].hdr.err   = ERR_NONE;
  }
  s_weather.hdr.state    = ST_LOADING;
  s_usage.hdr.state      = ST_LOADING;
  s_buddy.hdr.state      = ST_LOADING;
  s_nowplaying.hdr.state = ST_LOADING;
}

// --- success setters: copy payload, stamp LIVE + last_updated, clear err ---
static void mark_live(record_hdr_t* h) {
  h->state = ST_LIVE;
  h->err   = ERR_NONE;
  h->last_updated = s_now();
}

void ds_set_weather(const weather_rec_t* r) {
  ds_lock_take(&s_lock);
  s_weather = *r; mark_live(&s_weather.hdr);
  ds_lock_give(&s_lock);
}
void ds_set_finance(uint8_t idx, const finance_rec_t* r) {
  if (idx >= MAX_TICKERS) return;
  ds_lock_take(&s_lock);
  s_finance[idx] = *r; mark_live(&s_finance[idx].hdr);
  ds_lock_give(&s_lock);
}
void ds_set_usage(const usage_rec_t* r) {
  ds_lock_take(&s_lock);
  s_usage = *r; mark_live(&s_usage.hdr);
  ds_lock_give(&s_lock);
}
void ds_set_buddy(const buddy_rec_t* r) {
  ds_lock_take(&s_lock);
  s_buddy = *r; mark_live(&s_buddy.hdr);
  ds_lock_give(&s_lock);
}
void ds_set_nowplaying(const nowplaying_rec_t* r) {
  ds_lock_take(&s_lock);
  s_nowplaying = *r; mark_live(&s_nowplaying.hdr);
  ds_lock_give(&s_lock);
}

// --- explicit state setters: change state/err only, never the payload ---
static void set_state(record_hdr_t* h, screen_state_t s, data_err_t e) { h->state = s; h->err = e; }

void ds_set_state_weather(screen_state_t s, data_err_t e) {
  ds_lock_take(&s_lock); set_state(&s_weather.hdr, s, e); ds_lock_give(&s_lock);
}
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e) {
  if (idx >= MAX_TICKERS) return;
  ds_lock_take(&s_lock); set_state(&s_finance[idx].hdr, s, e); ds_lock_give(&s_lock);
}
void ds_set_state_usage(screen_state_t s, data_err_t e) {
  ds_lock_take(&s_lock); set_state(&s_usage.hdr, s, e); ds_lock_give(&s_lock);
}
void ds_set_state_buddy(screen_state_t s, data_err_t e) {
  ds_lock_take(&s_lock); set_state(&s_buddy.hdr, s, e); ds_lock_give(&s_lock);
}
void ds_set_state_nowplaying(screen_state_t s, data_err_t e) {
  ds_lock_take(&s_lock); set_state(&s_nowplaying.hdr, s, e); ds_lock_give(&s_lock);
}
void ds_set_hub_offline(void) {
  ds_lock_take(&s_lock);
  set_state(&s_usage.hdr, ST_HUB_OFFLINE, ERR_NONE);
  set_state(&s_buddy.hdr, ST_HUB_OFFLINE, ERR_NONE);
  ds_lock_give(&s_lock);
}

// --- getters: by-value snapshot under the lock ---
weather_rec_t ds_get_weather(void) {
  ds_lock_take(&s_lock); weather_rec_t c = s_weather; ds_lock_give(&s_lock); return c;
}
finance_rec_t ds_get_finance(uint8_t idx) {
  finance_rec_t c; memset(&c, 0, sizeof(c));
  if (idx >= MAX_TICKERS) return c;
  ds_lock_take(&s_lock); c = s_finance[idx]; ds_lock_give(&s_lock); return c;
}
uint8_t ds_get_finance_count(void) {
  ds_lock_take(&s_lock); uint8_t c = s_finance_count; ds_lock_give(&s_lock); return c;
}
usage_rec_t ds_get_usage(void) {
  ds_lock_take(&s_lock); usage_rec_t c = s_usage; ds_lock_give(&s_lock); return c;
}
buddy_rec_t ds_get_buddy(void) {
  ds_lock_take(&s_lock); buddy_rec_t c = s_buddy; ds_lock_give(&s_lock); return c;
}
nowplaying_rec_t ds_get_nowplaying(void) {
  ds_lock_take(&s_lock); nowplaying_rec_t c = s_nowplaying; ds_lock_give(&s_lock); return c;
}

// --- staleness sweep: ONLY ST_LIVE => ST_STALE at the inclusive boundary ---
static void sweep_one(record_hdr_t* h, uint32_t now, uint32_t stale_s) {
  if (h->state == ST_LIVE && record_age_s(h, now) >= stale_s) h->state = ST_STALE;
}
void ds_tick_staleness(uint32_t now) {
  ds_lock_take(&s_lock);
  sweep_one(&s_weather.hdr,    now, STALE_WEATHER_S);
  sweep_one(&s_usage.hdr,      now, STALE_USAGE_S);
  sweep_one(&s_buddy.hdr,      now, STALE_BUDDY_S);
  sweep_one(&s_nowplaying.hdr, now, STALE_NOWPLAYING_S);
  for (uint8_t i = 0; i < s_finance_count; i++)
    sweep_one(&s_finance[i].hdr, now, stale_finance_s(i));
  ds_lock_give(&s_lock);
}
```

- [ ] **Step 4.5: Write the FAILING tests FIRST (`test_datastore.cpp`, table-driven)**

```cpp
#include <unity.h>
#include <string.h>
#include "core/datastore.h"
#include "config/tickers.h"

static uint32_t g_clock = 0;
static uint32_t fake_now(void) { return g_clock; }

void setUp(void) { g_clock = 0; ds_set_clock(fake_now); datastore_init(); }
void tearDown(void) {}

static void test_init_seeds_finance_count_and_ids(void) {
  uint8_t n = ds_get_finance_count();
  uint8_t expect = (uint8_t)DEFAULT_TICKERS_COUNT;
  if (expect > MAX_TICKERS) expect = MAX_TICKERS;
  TEST_ASSERT_EQUAL_UINT8(expect, n);
  for (uint8_t i = 0; i < n; i++) {
    finance_rec_t f = ds_get_finance(i);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[i].id, f.id);   // slot i == ticker i
    TEST_ASSERT_EQUAL_INT(ST_LOADING, f.hdr.state);
  }
}

static void test_set_get_snapshot_equality(void) {
  g_clock = 5000;
  weather_rec_t w; memset(&w, 0, sizeof(w));
  w.temp_c = 29.5f; w.humidity_pct = 80.0f; w.wmo_code = 61;
  ds_set_weather(&w);
  weather_rec_t got = ds_get_weather();
  TEST_ASSERT_EQUAL_FLOAT(29.5f, got.temp_c);
  TEST_ASSERT_EQUAL_UINT16(61, got.wmo_code);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, got.hdr.state);
  TEST_ASSERT_EQUAL_UINT32(5000, got.hdr.last_updated);   // stamped from fake clock
}

static void test_finance_idx_isolation(void) {
  finance_rec_t a; memset(&a, 0, sizeof(a)); a.value = 111.0;
  ds_set_finance(0, &a);
  finance_rec_t slot1 = ds_get_finance(1);
  TEST_ASSERT_EQUAL_DOUBLE(0.0, slot1.value);             // writing slot 0 leaves slot 1 untouched
  TEST_ASSERT_EQUAL_INT(ST_LOADING, slot1.hdr.state);
  finance_rec_t slot0 = ds_get_finance(0);
  TEST_ASSERT_EQUAL_DOUBLE(111.0, slot0.value);
}

// Staleness sweep table: (initial state, last_updated, stale_s-source, now) -> expected state.
// Weather stale_s = STALE_WEATHER_S (1800). Boundary inclusive: age == stale_s => stale.
static void test_staleness_sweep_table(void) {
  static const struct {
    const char*    name;
    screen_state_t initial;
    data_err_t     err;
    uint32_t       last_updated;
    uint32_t       now;
    screen_state_t expect;
  } cases[] = {
    {"live below boundary stays live", ST_LIVE,  ERR_NONE, 1000, 1000 + 1799, ST_LIVE},
    {"live at boundary => stale",      ST_LIVE,  ERR_NONE, 1000, 1000 + 1800, ST_STALE},
    {"live past boundary => stale",    ST_LIVE,  ERR_NONE, 1000, 1000 + 5000, ST_STALE},
    {"error never clobbered",          ST_ERROR, ERR_HTTP, 1000, 1000 + 9999, ST_ERROR},
    {"offline never clobbered",        ST_OFFLINE, ERR_NONE, 1000, 1000 + 9999, ST_OFFLINE},
    {"loading not promoted",           ST_LOADING, ERR_NONE, 0,  9999, ST_LOADING},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    datastore_init();
    weather_rec_t w; memset(&w, 0, sizeof(w));
    g_clock = cases[i].last_updated;
    if (cases[i].initial == ST_LIVE) {
      ds_set_weather(&w);                                  // stamps last_updated + ST_LIVE
    } else {
      // force the non-live initial state + last_updated directly via setters
      ds_set_weather(&w);
      ds_set_state_weather(cases[i].initial, cases[i].err);
    }
    ds_tick_staleness(cases[i].now);
    weather_rec_t got = ds_get_weather();
    TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].expect, got.hdr.state, cases[i].name);
  }
}

static void test_hub_offline_flips_usage_and_buddy(void) {
  ds_set_hub_offline();
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_usage().hdr.state);
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_buddy().hdr.state);
}

static void test_success_clears_hub_offline_and_error(void) {
  ds_set_hub_offline();
  usage_rec_t u; memset(&u, 0, sizeof(u)); u.claude.h5.pct = 24;
  g_clock = 7000;
  ds_set_usage(&u);                                        // success clears HUB_OFFLINE
  usage_rec_t got = ds_get_usage();
  TEST_ASSERT_EQUAL_INT(ST_LIVE, got.hdr.state);
  TEST_ASSERT_EQUAL_INT16(24, got.claude.h5.pct);

  // explicit ERROR then a success clears it
  ds_set_state_usage(ST_ERROR, ERR_RATE_LIMITED);
  TEST_ASSERT_EQUAL_INT(ST_ERROR, ds_get_usage().hdr.state);
  ds_set_usage(&u);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, ds_get_usage().hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_NONE, ds_get_usage().hdr.err);
}

static void test_explicit_failure_preserves_payload(void) {
  g_clock = 3000;
  finance_rec_t f; memset(&f, 0, sizeof(f)); f.value = 42.0; strncpy(f.id, "btc", FIN_ID_LEN - 1);
  ds_set_finance(5, &f);
  ds_set_state_finance(5, ST_ERROR, ERR_TIMEOUT);         // failure must not touch payload
  finance_rec_t got = ds_get_finance(5);
  TEST_ASSERT_EQUAL_DOUBLE(42.0, got.value);              // value preserved
  TEST_ASSERT_EQUAL_STRING("btc", got.id);
  TEST_ASSERT_EQUAL_INT(ST_ERROR, got.hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_TIMEOUT, got.hdr.err);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_seeds_finance_count_and_ids);
  RUN_TEST(test_set_get_snapshot_equality);
  RUN_TEST(test_finance_idx_isolation);
  RUN_TEST(test_staleness_sweep_table);
  RUN_TEST(test_hub_offline_flips_usage_and_buddy);
  RUN_TEST(test_success_clears_hub_offline_and_error);
  RUN_TEST(test_explicit_failure_preserves_payload);
  return UNITY_END();
}
```

- [ ] **Step 4.6: Verify (host)** — run the test against an EMPTY `datastore.cpp` first (FAIL expected), then with the impl from Step 4.4:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native
```

Expected (after impl): all datastore tests PASS. The `build_src_filter` from Task 0 already links `datastore.cpp` into the native binary; `ds_lock.h` selects `std::mutex` because `BEACON_NATIVE` is defined.

- [ ] **Step 4.7: Commit checkpoint** —

```bash
git add firmware/beacon/src/core/stale.h firmware/beacon/src/core/ds_lock.h firmware/beacon/src/core/datastore.h firmware/beacon/src/core/datastore.cpp firmware/beacon/test/test_native/test_datastore.cpp
```

Suggested message: `feat(p0b): thread-safe DataStore with staleness sweep + state priority`

---

## Task 5: ui/theme.h + theme_catalog.h (catalog data + host lookup test)

`beacon_theme_t` (spec §4.1, with `radius`/`stroke_hair`/`stroke_med` added vs tech.md §6) + `gauge_style_t` + global space/motion `#define`s + the catalog. `lv_color_t`/`lv_font_t` make a full native compile hard, so split: a LVGL-free `theme_catalog.h` carries ids + colors as a host-shimmable `bt_rgb_t`; font pointers + `lv_style_t` building stay in `theme.cpp` (device-side). Host test asserts catalog count + ids match the DESIGN canonical set.

**Split rationale (described per the task brief):**
- **Host-testable (in `theme_catalog.h`):** the 7 theme `id` strings, gauge-style enum per theme, and the color tokens expressed as `bt_rgb_t {r,g,b}` (a plain struct, no LVGL). These are pure data and carry the DESIGN.md values.
- **Device-side only (in `theme.cpp`):** `THEMES[7]` of `beacon_theme_t` (which holds `lv_color_t` + `const lv_font_t*`), built by converting each `bt_rgb_t` via `lv_color_make()` and pointing at the generated fonts (Task 6). Font pointers and `lv_style_t` cannot link on native, so they never enter a native translation unit.

**Files:**
- Create: `firmware/beacon/src/ui/theme.h`
- Create: `firmware/beacon/src/ui/theme_catalog.h`
- Create: `firmware/beacon/test/test_native/test_theme_catalog.cpp`
- Modify: `firmware/beacon/platformio.ini` (add `theme_catalog.h` is header-only; no src filter change. But the native test includes it — no .cpp needed.)

- [ ] **Step 5.1: Write `theme.h` (spec §4.1 struct + §4.2 API + global tokens)**

```c
#pragma once
#include <stdint.h>
#ifndef BEACON_NATIVE
  #include <lvgl.h>
#endif

typedef enum {
  GAUGE_BAR, GAUGE_RING, GAUGE_CELL, GAUGE_WAVEFORM, GAUGE_MEASURE, GAUGE_BIGFIG, GAUGE_SUBDIAL
} gauge_style_t;

#ifndef BEACON_NATIVE
typedef struct {
  const char*      id;          // canonical id (DESIGN.md): editorial/hud/calm/blueprint/led/oscilloscope/analog
  lv_color_t       bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  const lv_font_t *f_display, *f_body, *f_mono;
  gauge_style_t    gauge;
  uint8_t          glow;        // 0..255 accent glow amount
  uint8_t          radius;      // element corner radius (px)  [ADDED vs tech.md §6]
  uint8_t          stroke_hair; // hairline width (px)         [ADDED]
  uint8_t          stroke_med;  // medium stroke width (px)    [ADDED]
} beacon_theme_t;
#endif

// Global (not per-theme) tokens, DESIGN.md. Space rhythm 4/8/12/16/24/32.
#define SPACE_1   4
#define SPACE_2   8
#define SPACE_3   12
#define SPACE_4   16
#define SPACE_5   24
#define SPACE_6   32

// Motion durations (ms); easing is a single shared ease-out-expo/quint for all transitions.
#define DUR_FAST  120
#define DUR       220
#define DUR_SLOW  400

#define THEME_COUNT 7
#define THEME_DEFAULT_IDX 0   // editorial

#ifndef BEACON_NATIVE
// Theme engine (theme.cpp). theme_set() runs the frozen teardown order (spec §4.2):
// destroy active screen -> free prior lv_style_t -> rebuild styles -> rebuild screen.
void theme_set(uint8_t idx);
const beacon_theme_t* theme_active(void);
uint8_t theme_active_idx(void);
#endif
```

(Global `space`/`motion` defines + `THEME_COUNT`/`THEME_DEFAULT_IDX` are host-visible — no LVGL needed — so they sit outside the `BEACON_NATIVE` guard. The struct, font pointers, and engine API need LVGL and are fenced off.)

- [ ] **Step 5.2: Write `theme_catalog.h` (LVGL-free catalog data with DESIGN.md values)**

Colors are the DESIGN.md token values (Editorial default table + catalog overrides). `bt_rgb_t` is a plain RGB triple so the catalog is host-testable; `theme.cpp` converts to `lv_color_t`. The `line` token in DESIGN is a translucent white (`rgba(255,255,255,.14)`) — flattened on pure-black canvas to its opaque equivalent `#242424` (0.14*255 ~= 36 = 0x24).

```c
#pragma once
#include <stdint.h>
#include "ui/theme.h"   // gauge_style_t, THEME_COUNT

typedef struct { uint8_t r, g, b; } bt_rgb_t;

// One catalog entry, LVGL-free. theme.cpp maps these to beacon_theme_t (lv_color_t + fonts).
typedef struct {
  const char*   id;
  bt_rgb_t      bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  gauge_style_t gauge;
  uint8_t       glow, radius, stroke_hair, stroke_med;
} theme_catalog_t;

// DESIGN.md token values. accent2 == accent where a theme uses a single accent.
static const theme_catalog_t THEME_CATALOG[THEME_COUNT] = {
  // editorial (default): signal orange, flat, hairline rules. up=ink, down=accent.
  {"editorial",
   {0,0,0}, {0xf4,0xf3,0xef}, {0x74,0x72,0x6c}, {0x24,0x24,0x24},
   {0xff,0x4a,0x2b}, {0xff,0x4a,0x2b}, {0xf4,0xf3,0xef}, {0xff,0x4a,0x2b}, {0xff,0x4a,0x2b},
   GAUGE_BAR, 0, 2, 1, 2},
  // hud: cyan + amber, concentric rings, subtle glow.
  {"hud",
   {0,0,0}, {0xe8,0xf4,0xf7}, {0x6a,0x82,0x88}, {0x14,0x2a,0x2e},
   {0x2d,0xe2,0xe6}, {0xff,0xb3,0x3a}, {0x2d,0xe2,0xe6}, {0xff,0xb3,0x3a}, {0xff,0xb3,0x3a},
   GAUGE_RING, 80, 4, 1, 2},
  // calm: faint red, sparse white-on-black, dot-matrix display.
  {"calm",
   {0,0,0}, {0xf2,0xf2,0xf2}, {0x6b,0x6b,0x6b}, {0x1e,0x1e,0x1e},
   {0xd6,0x4a,0x4a}, {0xd6,0x4a,0x4a}, {0xf2,0xf2,0xf2}, {0xd6,0x4a,0x4a}, {0xd6,0x4a,0x4a},
   GAUGE_BIGFIG, 0, 6, 1, 2},
  // blueprint: blueprint blue, draftsman line-art, dimensions.
  {"blueprint",
   {0,0,0}, {0xd6,0xe6,0xf5}, {0x5e,0x76,0x8c}, {0x16,0x2a,0x3e},
   {0x4a,0x9e,0xff}, {0x4a,0x9e,0xff}, {0xd6,0xe6,0xf5}, {0x4a,0x9e,0xff}, {0x4a,0x9e,0xff},
   GAUGE_MEASURE, 0, 0, 1, 2},
  // led: amber, dot panel, lit-pixel digits, slight glow.
  {"led",
   {0,0,0}, {0xff,0xc8,0x4a}, {0x6e,0x58,0x24}, {0x2a,0x22,0x0e},
   {0xff,0xb0,0x1a}, {0xff,0xb0,0x1a}, {0xff,0xc8,0x4a}, {0xff,0xb0,0x1a}, {0xff,0xb0,0x1a},
   GAUGE_CELL, 120, 2, 1, 2},
  // oscilloscope: phosphor green, graticule + signal trace, strong glow.
  {"oscilloscope",
   {0,0,0}, {0x4a,0xff,0x6a}, {0x2a,0x6e,0x38}, {0x10,0x28,0x16},
   {0x4a,0xff,0x6a}, {0x4a,0xff,0x6a}, {0x4a,0xff,0x6a}, {0xff,0x5a,0x5a}, {0xff,0xb0,0x1a},
   GAUGE_WAVEFORM, 160, 0, 1, 2},
  // analog: ice blue, analog hands + usage sub-dials.
  {"analog",
   {0,0,0}, {0xe6,0xf0,0xf7}, {0x6a,0x7c,0x88}, {0x1c,0x26,0x2c},
   {0x8a,0xc6,0xe6}, {0x8a,0xc6,0xe6}, {0xe6,0xf0,0xf7}, {0x8a,0xc6,0xe6}, {0xff,0xb0,0x1a},
   GAUGE_SUBDIAL, 0, 8, 1, 2},
};

// Canonical id order (DESIGN.md): index must equal THEME_CATALOG index.
static const char* const THEME_IDS[THEME_COUNT] = {
  "editorial", "hud", "calm", "blueprint", "led", "oscilloscope", "analog"
};
```

(Color values for the non-default themes are derived from DESIGN.md's catalog descriptions — "cyan + amber", "phosphor green", "ice blue", etc. — since DESIGN.md gives full per-token hex only for Editorial. These are the implementer's concrete realizations of the named accents on the shared pure-black/ink scheme; adjust on hardware in Task 9 if a hue reads wrong, but the ids/gauge/struct shape are frozen.)

- [ ] **Step 5.3: Write the FAILING test FIRST (`test_theme_catalog.cpp`, table-driven)**

```cpp
#include <unity.h>
#include <string.h>
#include "ui/theme_catalog.h"
#include "ui/theme.h"

void setUp(void) {}
void tearDown(void) {}

static void test_catalog_count(void) {
  TEST_ASSERT_EQUAL_INT(7, THEME_COUNT);
  // catalog array sized to THEME_COUNT
  TEST_ASSERT_EQUAL_INT(THEME_COUNT, (int)(sizeof(THEME_CATALOG) / sizeof(THEME_CATALOG[0])));
}

// ids match the DESIGN.md canonical set, in canonical order, default at index 0.
static void test_catalog_ids_canonical(void) {
  static const char* expect[] = {
    "editorial", "hud", "calm", "blueprint", "led", "oscilloscope", "analog"
  };
  for (int i = 0; i < THEME_COUNT; i++) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect[i], THEME_CATALOG[i].id, expect[i]);
    TEST_ASSERT_EQUAL_STRING(expect[i], THEME_IDS[i]);
  }
  TEST_ASSERT_EQUAL_STRING("editorial", THEME_CATALOG[THEME_DEFAULT_IDX].id);
}

// each entry's gauge is a valid gauge_style_t and bg is pure black (DESIGN: black is free).
static void test_catalog_invariants(void) {
  for (int i = 0; i < THEME_COUNT; i++) {
    const theme_catalog_t* t = &THEME_CATALOG[i];
    TEST_ASSERT_TRUE_MESSAGE(t->gauge <= GAUGE_SUBDIAL, "gauge out of range");
    TEST_ASSERT_EQUAL_UINT8(0, t->bg.r);
    TEST_ASSERT_EQUAL_UINT8(0, t->bg.g);
    TEST_ASSERT_EQUAL_UINT8(0, t->bg.b);
    TEST_ASSERT_NOT_NULL(t->id);
  }
}

// gauge assignment matches DESIGN.md catalog (editorial=bar ... analog=subdial).
static void test_catalog_gauge_mapping(void) {
  static const struct { const char* id; gauge_style_t gauge; } cases[] = {
    {"editorial", GAUGE_BAR},     {"hud", GAUGE_RING},      {"calm", GAUGE_BIGFIG},
    {"blueprint", GAUGE_MEASURE}, {"led", GAUGE_CELL},      {"oscilloscope", GAUGE_WAVEFORM},
    {"analog", GAUGE_SUBDIAL},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool found = false;
    for (int j = 0; j < THEME_COUNT; j++) {
      if (strcmp(THEME_CATALOG[j].id, cases[i].id) == 0) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].gauge, THEME_CATALOG[j].gauge, cases[i].id);
        found = true;
      }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, cases[i].id);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_catalog_count);
  RUN_TEST(test_catalog_ids_canonical);
  RUN_TEST(test_catalog_invariants);
  RUN_TEST(test_catalog_gauge_mapping);
  return UNITY_END();
}
```

- [ ] **Step 5.4: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: all theme-catalog tests PASS. `theme.h` compiles on native because the `lv_color_t`/`lv_font_t` struct + engine API are fenced behind `#ifndef BEACON_NATIVE`; only the enum + global defines are visible to the host.

- [ ] **Step 5.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/theme.h firmware/beacon/src/ui/theme_catalog.h firmware/beacon/test/test_native/test_theme_catalog.cpp
```

Suggested message: `feat(p0b): theme struct + global tokens + host-testable catalog`

---

## Task 6: Fonts — source, subset, generate, budget (FREEZE GATE)

Source the 7 display fonts + body/mono, generate LVGL C-array subsets via `npx lv_font_conv`, commit under `ui/fonts/`, produce the manifest, and confirm the app fits the 3MB `ota_0` slot. Device/tooling task — no Unity; verification is a build size check.

**Font roles (spec §4.3):** display = Space Grotesk, Rajdhani, Doto, Chakra Petch, Pixelify Sans, JetBrains Mono, Inter; shared body = Space Grotesk / Rajdhani / Chakra Petch / Inter (per theme); shared mono = JetBrains Mono (all). To bound flash, each display family ships TWO subsets: a mid size (full printable ASCII + `°`, for now-playing track title + labels) and a hero size (digits + `:%°.,+-/` only, for clock / big %).

**Files:**
- Create: `firmware/beacon/src/ui/fonts/*.c` (generated)
- Create: `firmware/beacon/src/ui/fonts/fonts.h` (LV_FONT_DECLARE for all subsets)
- Create: `firmware/beacon/src/ui/fonts/MANIFEST.md` (the asset/flash budget)
- Modify: `firmware/beacon/src/lv_conf.h` (add `LV_FONT_CUSTOM_DECLARE` entries so LVGL knows the fonts)

- [ ] **Step 6.1: Get the TTFs (download into a scratch dir, do NOT commit the TTFs)**

```bash
mkdir -p /tmp/beacon-fonts && cd /tmp/beacon-fonts
# Google Fonts (OFL) — fetch the static TTFs:
#   Space Grotesk, Rajdhani, Doto, Chakra Petch, Pixelify Sans, JetBrains Mono, Inter
# Example (repeat per family; use the static, not variable, TTF where a weight is named):
#   curl -L -o SpaceGrotesk-Medium.ttf  "<google-fonts raw TTF url>"
#   curl -L -o JetBrainsMono-Regular.ttf "<google-fonts raw TTF url>"
# Source weights per DESIGN.md: Space Grotesk 500, Inter Light/400, JetBrains Mono 400-500,
# Rajdhani 500, Doto (dot), Chakra Petch 500, Pixelify Sans 400.
```

(Fonts are OFL — license permits embedding. Keep TTFs out of the repo; only the generated `.c` arrays are committed. If offline, the user supplies the TTFs in `/tmp/beacon-fonts`.)

- [ ] **Step 6.2: Generate the LVGL C-array subsets with `npx lv_font_conv`**

Two glyph profiles. `--range 0x20-0x7F` is full printable ASCII; `0xB0` is `°`. Hero subset is digits + a few symbols via `--symbols`. `--bpp 4` for smooth display fonts, `--bpp 1` for the LED/pixel + oscilloscope styles (crisp, smaller). `--format lvgl --lv-include lvgl.h`.

```bash
cd /tmp/beacon-fonts
OUT=/Users/angaziz/work/personal/beacon/firmware/beacon/src/ui/fonts

# --- shared body/mono (full ASCII + degree), used by labels/rows/track/artist/errors ---
npx lv_font_conv --font JetBrainsMono-Regular.ttf --size 18 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --force-fast-kern-format \
  --lv-include lvgl.h -o $OUT/font_mono_18.c
npx lv_font_conv --font JetBrainsMono-Regular.ttf --size 14 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --force-fast-kern-format \
  --lv-include lvgl.h -o $OUT/font_mono_14.c

# --- per-theme display MID size (28px, full ASCII + degree: track titles + headings) ---
# editorial: Space Grotesk Medium
npx lv_font_conv --font SpaceGrotesk-Medium.ttf --size 28 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_editorial_28.c
# hud: Rajdhani
npx lv_font_conv --font Rajdhani-Medium.ttf --size 28 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_hud_28.c
# calm: Doto (dot-matrix; bpp 1 keeps the dot look + small)
npx lv_font_conv --font Doto-Regular.ttf --size 28 --bpp 1 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_calm_28.c
# blueprint: Chakra Petch
npx lv_font_conv --font ChakraPetch-Medium.ttf --size 28 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_blueprint_28.c
# led: Pixelify Sans (pixel; bpp 1)
npx lv_font_conv --font PixelifySans-Regular.ttf --size 28 --bpp 1 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_led_28.c
# oscilloscope: JetBrains Mono (bpp 4, glow drawn separately)
npx lv_font_conv --font JetBrainsMono-Regular.ttf --size 28 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_oscilloscope_28.c
# analog: Inter Light
npx lv_font_conv --font Inter-Light.ttf --size 28 --bpp 4 \
  --range 0x20-0x7F --range 0xB0 --format lvgl --lv-include lvgl.h \
  -o $OUT/disp_analog_28.c

# --- per-theme HERO size (72px, digits + clock/finance symbols only: clock + big %) ---
# symbols restrict the heavy glyphs to what hero figures use.
HERO_SYMS="0123456789:%.,+-/ °"
for pair in \
  "editorial:SpaceGrotesk-Medium.ttf:4" \
  "hud:Rajdhani-Medium.ttf:4" \
  "calm:Doto-Regular.ttf:1" \
  "blueprint:ChakraPetch-Medium.ttf:4" \
  "led:PixelifySans-Regular.ttf:1" \
  "oscilloscope:JetBrainsMono-Regular.ttf:4" \
  "analog:Inter-Light.ttf:4"; do
  name="${pair%%:*}"; rest="${pair#*:}"; ttf="${rest%%:*}"; bpp="${rest##*:}"
  npx lv_font_conv --font "$ttf" --size 72 --bpp "$bpp" \
    --symbols "$HERO_SYMS" --format lvgl --lv-include lvgl.h \
    -o "$OUT/disp_${name}_72.c"
done
```

(Body fonts: editorial/oscilloscope reuse their display family at body size, hud uses Rajdhani, blueprint uses Chakra Petch, calm/led/analog use Inter. To keep this task bounded, the shared `font_mono_18/14` cover mono + double as the generic body where a theme's body == mono is acceptable; per-theme body at 18px is generated the same way as the MID display block above if Task 9 shows a theme needs its own body weight. The MANIFEST records exactly which subsets shipped.)

- [ ] **Step 6.3: Write `fonts.h` (declare every generated subset)**

```c
#pragma once
#include <lvgl.h>

LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_mono_14);

LV_FONT_DECLARE(disp_editorial_28);   LV_FONT_DECLARE(disp_editorial_72);
LV_FONT_DECLARE(disp_hud_28);         LV_FONT_DECLARE(disp_hud_72);
LV_FONT_DECLARE(disp_calm_28);        LV_FONT_DECLARE(disp_calm_72);
LV_FONT_DECLARE(disp_blueprint_28);   LV_FONT_DECLARE(disp_blueprint_72);
LV_FONT_DECLARE(disp_led_28);         LV_FONT_DECLARE(disp_led_72);
LV_FONT_DECLARE(disp_oscilloscope_28);LV_FONT_DECLARE(disp_oscilloscope_72);
LV_FONT_DECLARE(disp_analog_28);      LV_FONT_DECLARE(disp_analog_72);
```

- [ ] **Step 6.4: Point lv_conf.h at the custom fonts**

Edit the existing `LV_FONT_CUSTOM_DECLARE` line (currently empty at lv_conf.h:399). LVGL declares custom fonts here so they are visible to its font subsystem:

```c
#define LV_FONT_CUSTOM_DECLARE \
  LV_FONT_DECLARE(font_mono_18) LV_FONT_DECLARE(font_mono_14) \
  LV_FONT_DECLARE(disp_editorial_28) LV_FONT_DECLARE(disp_editorial_72) \
  LV_FONT_DECLARE(disp_hud_28) LV_FONT_DECLARE(disp_hud_72) \
  LV_FONT_DECLARE(disp_calm_28) LV_FONT_DECLARE(disp_calm_72) \
  LV_FONT_DECLARE(disp_blueprint_28) LV_FONT_DECLARE(disp_blueprint_72) \
  LV_FONT_DECLARE(disp_led_28) LV_FONT_DECLARE(disp_led_72) \
  LV_FONT_DECLARE(disp_oscilloscope_28) LV_FONT_DECLARE(disp_oscilloscope_72) \
  LV_FONT_DECLARE(disp_analog_28) LV_FONT_DECLARE(disp_analog_72)
```

- [ ] **Step 6.5: Write `MANIFEST.md` (the font/asset budget — fill measured bytes after the build)**

Create `firmware/beacon/src/ui/fonts/MANIFEST.md` with one row per subset (family, role, size, bpp, glyph set, bytes) plus the budget summary. Bytes are the `.c` array sizes (measure with `wc -c` and/or the link map). Template:

```
| File | Family | Role | Size | bpp | Glyphs | Bytes |
|---|---|---|---|---|---|---|
| font_mono_18.c        | JetBrains Mono | body+mono | 18 | 4 | ASCII+deg | <measure> |
| font_mono_14.c        | JetBrains Mono | body+mono | 14 | 4 | ASCII+deg | <measure> |
| disp_editorial_28.c   | Space Grotesk  | display   | 28 | 4 | ASCII+deg | <measure> |
| disp_editorial_72.c   | Space Grotesk  | hero      | 72 | 4 | digits+sym| <measure> |
| ... (one row per generated subset) ...
| **Total fonts** | | | | | | **<sum>** |

App slot budget: ota_0 = 0x300000 = 3145728 B.
P0-A baseline app = ~647 KB. Fonts total = <sum>. Headroom = 3MB - (647KB + <sum>).
FREEZE GATE: PASS iff measured firmware .bin Flash% < 100% of ota_0 with margin.
```

- [ ] **Step 6.6: Verify (FREEZE GATE — device build size)** — build and read the Flash summary:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon
```

(Note: fonts only link in once `theme.cpp`/`gauge.cpp` from Tasks 7-8 reference them; this build links them via `LV_FONT_CUSTOM_DECLARE` + a throwaway reference if needed. If the linker drops unused arrays, defer the size gate to Task 9's build where the demo references all 7 themes.) Expected: build succeeds; PlatformIO prints `Flash: [== ] xx.x% (used N bytes from 3145728 bytes)`. **GATE: the used bytes must be < 3145728 (ota_0) with comfortable margin.** Record N in MANIFEST.md and confirm PASS.

- [ ] **Step 6.7: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/fonts firmware/beacon/src/lv_conf.h
```

Suggested message: `feat(p0b): glyph-subset theme fonts + flash-budget manifest`

---

## Task 7: ui/theme.cpp — theme_set() (frozen teardown order) + theme_active()

Build `THEMES[7]` from `THEME_CATALOG` + the fonts; implement `theme_set()` in the spec §4.2 frozen order and `theme_active()`/`theme_active_idx()`. Device task.

**Files:**
- Create: `firmware/beacon/src/ui/theme.cpp`

- [ ] **Step 7.1: Write `theme.cpp`**

`theme_set()` follows the frozen order: (1) destroy the active screen's objects, (2) free prior `lv_style_t`, (3) rebuild the shared style set from the new tokens, (4) rebuild the active screen. The screen-rebuild callback is injected (`theme_on_rebuild`) so the engine does not hardcode any one screen — the demo (Task 9) and later real screens register theirs.

```cpp
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/fonts/fonts.h"
#include "util/log.h"
#include <lvgl.h>

// rgb triple -> lv_color_t (RGB565 packed, LV_COLOR_DEPTH=16).
static inline lv_color_t mk(bt_rgb_t c) { return lv_color_make(c.r, c.g, c.b); }

// Per-theme font selection: {display-hero, display-mid, body, mono}.
typedef struct { const lv_font_t *hero, *mid, *body, *mono; } theme_fonts_t;
static const theme_fonts_t THEME_FONTS[THEME_COUNT] = {
  {&disp_editorial_72,    &disp_editorial_28,    &font_mono_18, &font_mono_18}, // editorial
  {&disp_hud_72,          &disp_hud_28,          &font_mono_18, &font_mono_18}, // hud
  {&disp_calm_72,         &disp_calm_28,         &font_mono_18, &font_mono_18}, // calm
  {&disp_blueprint_72,    &disp_blueprint_28,    &font_mono_18, &font_mono_18}, // blueprint
  {&disp_led_72,          &disp_led_28,          &font_mono_18, &font_mono_18}, // led
  {&disp_oscilloscope_72, &disp_oscilloscope_28, &font_mono_18, &font_mono_18}, // oscilloscope
  {&disp_analog_72,       &disp_analog_28,       &font_mono_18, &font_mono_18}, // analog
};

static beacon_theme_t s_themes[THEME_COUNT];
static bool           s_built = false;
static uint8_t        s_active = THEME_DEFAULT_IDX;

// shared style set, rebuilt per theme; freed before each rebuild (one-theme-resident rule).
static lv_style_t s_st_screen, s_st_eyebrow, s_st_display, s_st_body, s_st_value;
static bool       s_styles_inited = false;

// screen-rebuild hook (injected by the active screen module).
typedef void (*theme_rebuild_fn)(void);
static theme_rebuild_fn s_rebuild = nullptr;
void theme_on_rebuild(theme_rebuild_fn fn) { s_rebuild = fn; }   // declared in theme.h (device side)

static void build_catalog_once(void) {
  if (s_built) return;
  for (int i = 0; i < THEME_COUNT; i++) {
    const theme_catalog_t* c = &THEME_CATALOG[i];
    beacon_theme_t* t = &s_themes[i];
    t->id      = c->id;
    t->bg      = mk(c->bg);      t->ink     = mk(c->ink);     t->ink_dim = mk(c->ink_dim);
    t->line    = mk(c->line);    t->accent  = mk(c->accent);  t->accent2 = mk(c->accent2);
    t->up      = mk(c->up);      t->down    = mk(c->down);    t->alert   = mk(c->alert);
    t->f_display = THEME_FONTS[i].mid;     // mid carries full ASCII for titles/labels
    t->f_body    = THEME_FONTS[i].body;
    t->f_mono    = THEME_FONTS[i].mono;
    t->gauge   = c->gauge;
    t->glow    = c->glow;        t->radius  = c->radius;
    t->stroke_hair = c->stroke_hair;       t->stroke_med = c->stroke_med;
  }
  s_built = true;
}

// step 2: release prior lv_style_t (idempotent).
static void free_styles(void) {
  if (!s_styles_inited) return;
  lv_style_reset(&s_st_screen);  lv_style_reset(&s_st_eyebrow); lv_style_reset(&s_st_display);
  lv_style_reset(&s_st_body);    lv_style_reset(&s_st_value);
  s_styles_inited = false;
}

// step 3: rebuild the shared style set from the active theme's tokens.
static void build_styles(const beacon_theme_t* t) {
  lv_style_init(&s_st_screen);
  lv_style_set_bg_color(&s_st_screen, t->bg);
  lv_style_set_bg_opa(&s_st_screen, LV_OPA_COVER);
  lv_style_set_text_color(&s_st_screen, t->ink);
  lv_style_set_pad_all(&s_st_screen, SPACE_4);
  lv_style_set_radius(&s_st_screen, t->radius);

  lv_style_init(&s_st_eyebrow);
  lv_style_set_text_font(&s_st_eyebrow, t->f_mono);
  lv_style_set_text_color(&s_st_eyebrow, t->ink_dim);

  lv_style_init(&s_st_display);
  lv_style_set_text_font(&s_st_display, t->f_display);
  lv_style_set_text_color(&s_st_display, t->ink);

  lv_style_init(&s_st_body);
  lv_style_set_text_font(&s_st_body, t->f_body);
  lv_style_set_text_color(&s_st_body, t->ink);

  lv_style_init(&s_st_value);
  lv_style_set_text_font(&s_st_value, t->f_mono);
  lv_style_set_text_color(&s_st_value, t->accent);

  s_styles_inited = true;
}

// exposed for screens that style their own objects from the shared set.
lv_style_t* theme_style_screen(void)  { return &s_st_screen;  }
lv_style_t* theme_style_eyebrow(void) { return &s_st_eyebrow; }
lv_style_t* theme_style_display(void) { return &s_st_display; }
lv_style_t* theme_style_body(void)    { return &s_st_body;    }
lv_style_t* theme_style_value(void)   { return &s_st_value;   }

void theme_set(uint8_t idx) {
  if (idx >= THEME_COUNT) idx = THEME_DEFAULT_IDX;
  build_catalog_once();
  s_active = idx;

  // 1) destroy/detach the active screen's objects (clean current tree first).
  lv_obj_t* scr = lv_scr_act();
  if (scr) lv_obj_clean(scr);

  // 2) release the prior theme's lv_style_t (one-theme-resident; tech.md §6).
  free_styles();

  // 3) rebuild the shared style set from the new theme's tokens.
  build_styles(&s_themes[idx]);

  // 4) rebuild the active screen from the new styles (no reboot; FR-THEME-3).
  lv_obj_add_style(scr, &s_st_screen, 0);
  if (s_rebuild) s_rebuild();

  LOGI("theme set => %s (idx %u)", s_themes[idx].id, (unsigned)idx);
}

const beacon_theme_t* theme_active(void) { build_catalog_once(); return &s_themes[s_active]; }
uint8_t theme_active_idx(void) { return s_active; }
```

- [ ] **Step 7.2: Add the device-side decls to `theme.h`** (inside the existing `#ifndef BEACON_NATIVE` engine block):

```c
typedef void (*theme_rebuild_fn)(void);
void theme_on_rebuild(theme_rebuild_fn fn);
lv_style_t* theme_style_screen(void);
lv_style_t* theme_style_eyebrow(void);
lv_style_t* theme_style_display(void);
lv_style_t* theme_style_body(void);
lv_style_t* theme_style_value(void);
```

- [ ] **Step 7.3: Verify (device build)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon`. Expected: links clean (theme.cpp + fonts compile into the image). No runtime check yet — that is Task 9.

- [ ] **Step 7.4: Verify native is untouched** — `~/.beacon-pio/bin/pio test -e native`. Expected: still all PASS (theme.cpp is NOT in `build_src_filter`, so native ignores it).

- [ ] **Step 7.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/theme.cpp firmware/beacon/src/ui/theme.h
```

Suggested message: `feat(p0b): theme engine - theme_set frees prior styles then rebuilds (FR-THEME-1/3)`

---

## Task 8: ui/gauge.{h,cpp} — gauge_render() (bar full; others concrete skeletons)

`gauge_render()` switches on the active theme's `gauge_style`. Implement `GAUGE_BAR` in full; `ring/cell/measure/bigfig` as concrete LVGL 8.4 skeletons; `waveform/subdial` render a labeled deferred stub. Device task.

**Files:**
- Create: `firmware/beacon/src/ui/gauge.h`
- Create: `firmware/beacon/src/ui/gauge.cpp`

- [ ] **Step 8.1: Write `gauge.h`**

```c
#pragma once
#ifndef BEACON_NATIVE
#include <lvgl.h>

// Renders a 0..1 normalized level into `parent` using the ACTIVE theme's gauge_style.
// Returns the created container (caller positions/sizes it). pct is clamped to [0,1].
lv_obj_t* gauge_render(lv_obj_t* parent, float pct);
#endif
```

- [ ] **Step 8.2: Write `gauge.cpp` (bar full; others concrete)**

```cpp
#include "ui/gauge.h"
#include "ui/theme.h"
#include "util/log.h"
#include <lvgl.h>

#define GAUGE_W      200
#define GAUGE_H       16
#define GAUGE_CELLS   20     // cell-style segment count
#define RING_SIZE    120
#define RING_ARC_W     8

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// --- GAUGE_BAR (Editorial): hairline track + accent fill, full implementation ---
static lv_obj_t* render_bar(lv_obj_t* parent, float pct, const beacon_theme_t* t) {
  lv_obj_t* track = lv_obj_create(parent);
  lv_obj_set_size(track, GAUGE_W, GAUGE_H);
  lv_obj_set_style_bg_opa(track, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(track, t->radius, 0);
  lv_obj_set_style_border_width(track, t->stroke_hair, 0);
  lv_obj_set_style_border_color(track, t->line, 0);
  lv_obj_set_style_pad_all(track, 0, 0);
  lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* fill = lv_obj_create(track);
  lv_obj_set_size(fill, (lv_coord_t)(GAUGE_W * clamp01(pct)), GAUGE_H);
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(fill, t->accent, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(fill, t->radius, 0);
  lv_obj_set_style_border_width(fill, 0, 0);
  if (t->glow) {                                  // optional accent glow (themes with glow>0)
    lv_obj_set_style_shadow_color(fill, t->accent, 0);
    lv_obj_set_style_shadow_width(fill, t->glow / 8, 0);
    lv_obj_set_style_shadow_opa(fill, LV_OPA_50, 0);
  }
  return track;
}

// --- GAUGE_RING (HUD): concentric arc, accent foreground over line track ---
static lv_obj_t* render_ring(lv_obj_t* parent, float pct, const beacon_theme_t* t) {
  lv_obj_t* arc = lv_arc_create(parent);
  lv_obj_set_size(arc, RING_SIZE, RING_SIZE);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, (int16_t)(clamp01(pct) * 100));
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);   // no draggable knob
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, t->line,   LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, RING_ARC_W, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, t->accent, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, RING_ARC_W, LV_PART_INDICATOR);
  return arc;
}

// --- GAUGE_CELL (LED): N segments, lit up to pct in accent, rest in line ---
static lv_obj_t* render_cell(lv_obj_t* parent, float pct, const beacon_theme_t* t) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, GAUGE_W, GAUGE_H);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, SPACE_1, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  int lit = (int)(clamp01(pct) * GAUGE_CELLS + 0.5f);
  for (int i = 0; i < GAUGE_CELLS; i++) {
    lv_obj_t* c = lv_obj_create(row);
    lv_obj_set_size(c, (GAUGE_W / GAUGE_CELLS) - SPACE_1, GAUGE_H);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, i < lit ? t->accent : t->line, 0);
  }
  return row;
}

// --- GAUGE_MEASURE (Blueprint): a ruler tick scale with an accent index mark ---
static lv_obj_t* render_measure(lv_obj_t* parent, float pct, const beacon_theme_t* t) {
  lv_obj_t* scale = lv_obj_create(parent);
  lv_obj_set_size(scale, GAUGE_W, GAUGE_H + SPACE_2);
  lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(scale, 0, 0);
  lv_obj_set_style_border_side(scale, LV_BORDER_SIDE_BOTTOM, 0); // baseline rule
  lv_obj_set_style_border_width(scale, t->stroke_hair, 0);
  lv_obj_set_style_border_color(scale, t->line, 0);
  lv_obj_clear_flag(scale, LV_OBJ_FLAG_SCROLLABLE);
  // index mark at pct
  lv_obj_t* idx = lv_obj_create(scale);
  lv_obj_set_size(idx, t->stroke_med, GAUGE_H);
  lv_obj_align(idx, LV_ALIGN_BOTTOM_LEFT, (lv_coord_t)(GAUGE_W * clamp01(pct)), 0);
  lv_obj_set_style_bg_color(idx, t->accent, 0);
  lv_obj_set_style_bg_opa(idx, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(idx, 0, 0);
  return scale;
}

// --- GAUGE_BIGFIG (Calm): the value AS a big figure, no track ---
static lv_obj_t* render_bigfig(lv_obj_t* parent, float pct, const beacon_theme_t* t) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_add_style(lbl, theme_style_display(), 0);   // hero/display font from active theme
  lv_label_set_text_fmt(lbl, "%d%%", (int)(clamp01(pct) * 100 + 0.5f));
  lv_obj_set_style_text_color(lbl, t->accent, 0);
  return lbl;
}

// --- deferred bespoke styles: labeled placeholder (need custom widgets; DESIGN.md outliers) ---
static lv_obj_t* render_deferred(lv_obj_t* parent, const char* name, const beacon_theme_t* t) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_add_style(lbl, theme_style_eyebrow(), 0);
  lv_label_set_text_fmt(lbl, "%s: deferred", name);
  lv_obj_set_style_text_color(lbl, t->ink_dim, 0);
  return lbl;
}

lv_obj_t* gauge_render(lv_obj_t* parent, float pct) {
  const beacon_theme_t* t = theme_active();
  switch (t->gauge) {
    case GAUGE_BAR:     return render_bar(parent, pct, t);
    case GAUGE_RING:    return render_ring(parent, pct, t);
    case GAUGE_CELL:    return render_cell(parent, pct, t);
    case GAUGE_MEASURE: return render_measure(parent, pct, t);
    case GAUGE_BIGFIG:  return render_bigfig(parent, pct, t);
    case GAUGE_WAVEFORM: return render_deferred(parent, "waveform", t);
    case GAUGE_SUBDIAL:  return render_deferred(parent, "subdial",  t);
  }
  return render_deferred(parent, "unknown", t);
}
```

- [ ] **Step 8.3: Verify (device build)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon`. Expected: links clean.

- [ ] **Step 8.4: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/gauge.h firmware/beacon/src/ui/gauge.cpp
```

Suggested message: `feat(p0b): token-driven gauge component (bar/ring/cell/measure/bigfig; waveform/subdial deferred) (FR-THEME-4)`

---

## Task 9: on-device verification — theme-switch demo + DataStore smoke

A throwaway demo screen (tap to cycle all 7 themes; eyebrow + big figure + a gauge + a finance-style row) + a Core-0 DataStore smoke task (mock writes + state transitions logged). Flash + verify all 7 themes render correctly, live switch with no reboot (FR-THEME-3), heap floor holds across switches. Device task — replaces the P0-A test screen in `main.cpp` for this chunk's verification.

**Files:**
- Create: `firmware/beacon/src/ui/demo_screen.h`
- Create: `firmware/beacon/src/ui/demo_screen.cpp`
- Modify: `firmware/beacon/src/main.cpp` (swap `test_screen_show()` for the demo + start the smoke task)

- [ ] **Step 9.1: Write `demo_screen.h`**

```c
#pragma once
// Throwaway P0-B verification screen: tap to cycle all 7 themes; renders eyebrow + big figure
// + gauge + a finance-style row from the ACTIVE theme's tokens. Removed when real screens land.
void demo_screen_show(void);
```

- [ ] **Step 9.2: Write `demo_screen.cpp`**

```cpp
#include "ui/demo_screen.h"
#include "ui/theme.h"
#include "ui/gauge.h"
#include "config/layout.h"
#include "util/log.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

static lv_obj_t* s_safe = nullptr;

// builds the representative layout into the active screen from current theme styles.
static void demo_rebuild(void) {
  const beacon_theme_t* t = theme_active();
  lv_obj_t* scr = lv_scr_act();

  s_safe = lv_obj_create(scr);
  lv_obj_set_size(s_safe, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET);
  lv_obj_center(s_safe);
  lv_obj_set_style_bg_opa(s_safe, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_safe, 0, 0);
  lv_obj_set_flex_flow(s_safe, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_safe, SPACE_4, 0);
  lv_obj_clear_flag(s_safe, LV_OBJ_FLAG_SCROLLABLE);

  // eyebrow: mono BEACON / <THEME>
  lv_obj_t* eb = lv_label_create(s_safe);
  lv_obj_add_style(eb, theme_style_eyebrow(), 0);
  lv_label_set_text_fmt(eb, "BEACON / %s", t->id);

  // big figure (display/hero font)
  lv_obj_t* fig = lv_label_create(s_safe);
  lv_obj_add_style(fig, theme_style_display(), 0);
  lv_label_set_text(fig, "42%");
  lv_obj_set_style_text_color(fig, t->accent, 0);

  // gauge (token-driven)
  gauge_render(s_safe, 0.42f);

  // finance-style row: label (body) + value (mono/accent)
  lv_obj_t* row = lv_obj_create(s_safe);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, t->stroke_hair, 0);
  lv_obj_set_style_border_color(row, t->line, 0);
  lv_obj_set_style_pad_all(row, SPACE_2, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* k = lv_label_create(row);
  lv_obj_add_style(k, theme_style_body(), 0);
  lv_label_set_text(k, "BTC");
  lv_obj_t* v = lv_label_create(row);
  lv_obj_add_style(v, theme_style_value(), 0);
  lv_label_set_text(v, "+2.4%");
  lv_obj_set_style_text_color(v, t->up, 0);
}

static void tap_cb(lv_event_t*) {
  uint8_t next = (theme_active_idx() + 1) % THEME_COUNT;
  uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  theme_set(next);                                    // frees prior styles, rebuilds via demo_rebuild
  uint32_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  LOGI("theme switch -> %s; free internal heap %u (was %u)",
       theme_active()->id, (unsigned)after, (unsigned)before);
}

void demo_screen_show(void) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  theme_on_rebuild(demo_rebuild);                    // engine calls this on every theme_set
  theme_set(THEME_DEFAULT_IDX);                      // first paint = editorial
}
```

- [ ] **Step 9.3: Add a DataStore smoke task + wire the demo in `main.cpp`**

Replace the `test_screen` include + call, add the datastore + demo includes, install an epoch clock, and start a Core-0 FreeRTOS task that drives mock writes + state transitions. Surgical edits to the existing `main.cpp`:

Change the includes block (drop `test_screen.h`, add the new ones):

```cpp
#include <Arduino.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "ui/demo_screen.h"
#include "core/datastore.h"
#include "config/tickers.h"
#include <time.h>
```

Add the clock source + smoke task above `setup()`:

```cpp
static uint32_t epoch_now(void) { return (uint32_t)time(nullptr); }   // RTC/NTP wired in P0 time service

// Core-0 smoke task: mock writes + LIVE=>STALE=>ERROR=>recover, logs the sweep + priority rules.
static void smoke_task(void*) {
  uint32_t base = epoch_now();
  for (;;) {
    finance_rec_t f = {}; f.value = 68000.0; f.change_pct = 2.4;
    strncpy(f.id, DEFAULT_TICKERS[5].id, FIN_ID_LEN - 1);
    ds_set_finance(5, &f);                              // LIVE (btc slot)
    LOGI("smoke: btc set LIVE state=%d", ds_get_finance(5).hdr.state);

    ds_tick_staleness(epoch_now());                    // not stale yet
    ds_tick_staleness(epoch_now() + 700);              // > btc stale_s (600) => STALE
    LOGI("smoke: after sweep state=%d (expect %d STALE)", ds_get_finance(5).hdr.state, ST_STALE);

    ds_set_state_finance(5, ST_ERROR, ERR_RATE_LIMITED);
    LOGI("smoke: forced ERROR state=%d err=%d", ds_get_finance(5).hdr.state, ds_get_finance(5).hdr.err);

    ds_set_hub_offline();
    LOGI("smoke: hub offline usage=%d buddy=%d", ds_get_usage().hdr.state, ds_get_buddy().hdr.state);
    usage_rec_t u = {}; u.claude.h5.pct = 24; ds_set_usage(&u);
    LOGI("smoke: usage recovered state=%d (expect %d LIVE)", ds_get_usage().hdr.state, ST_LIVE);

    ds_set_finance(5, &f);                              // recover btc
    LOGI("smoke: btc recovered state=%d", ds_get_finance(5).hdr.state);
    (void)base;
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
```

In `setup()`, after `lvgl_port_begin()` succeeds, replace `test_screen_show();` with:

```cpp
  ds_set_clock(epoch_now);
  datastore_init();
  demo_screen_show();
  xTaskCreatePinnedToCore(smoke_task, "smoke", 4096, nullptr, 1, nullptr, 0);  // Core 0
```

- [ ] **Step 9.4: Verify (device — flash + observe)** — flash and monitor:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon -t upload && ~/.beacon-pio/bin/pio device monitor
```

Expected on the PANEL: editorial theme paints first (eyebrow `BEACON / editorial`, big `42%`, a hairline bar gauge, a `BTC +2.4%` row). Each TAP cycles to the next theme — all 7 render with the correct display font + accent color + gauge style (bar=>ring=>bigfig=>measure=>cell=>waveform-stub=>subdial-stub), no reboot, no corruption (FR-THEME-2 + FR-THEME-3).

Expected on SERIAL:
- `theme set => <id> (idx N)` per switch and `theme switch -> <id>; free internal heap U (was W)` — the heap floor must HOLD across many switches (U does not monotonically fall; proves `theme_set` frees prior styles).
- smoke task: `btc set LIVE`, `after sweep state=2 (expect 2 STALE)`, `forced ERROR`, `hub offline usage=5 buddy=5`, `usage recovered state=1`, `btc recovered`.
- The `>= 60 KB free internal heap` floor (`tech.md` §8) is never breached during switching.

If any theme renders wrong glyphs (missing chars) revisit the font subset ranges (Task 6); if heap falls per switch, `free_styles()` is not resetting all styles.

- [ ] **Step 9.5: Re-confirm the FREEZE GATE with the full demo build** — the demo references all 7 themes + all fonts, so the linker keeps every array. Read the Flash% from the Step 9.4 build and finalize MANIFEST.md: confirm `used < 3145728` (ota_0) with margin. GATE must be PASS before the chunk is "done".

- [ ] **Step 9.6: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/demo_screen.h firmware/beacon/src/ui/demo_screen.cpp firmware/beacon/src/main.cpp firmware/beacon/src/ui/fonts/MANIFEST.md
```

Suggested message: `feat(p0b): on-device theme-switch demo + DataStore smoke; confirm flash freeze gate (FR-THEME-2/3)`

---

## Self-review

### Spec coverage checklist

| Spec item | Covered by |
|---|---|
| §3.1 `screen_state_t` + `data_err_t` + state-priority rule | Task 1 (verbatim) + Task 4 sweep table |
| §3.2 record_hdr_t + 5 records + string-length defines + art-ref + `record_age_s` | Task 1 (verbatim) + `test_records` |
| §3.3 DataStore get/set/sweep API + finance_count init contract | Task 4 |
| §3.4 HubLink interface + send/callback/copy semantics | Task 3 (verbatim) + mock test |
| §3.5 tickers.h (DEFAULT_TICKERS, MAX_TICKERS, enums) | Task 2 (verbatim) + config test |
| §3.5 location.h (location_cfg_t, DEFAULT_LOCATION, wmo_entry_t, WMO_MAP) | Task 2 |
| §4.1 beacon_theme_t (+radius/stroke_hair/stroke_med) + gauge_style_t + global space/motion | Task 5 (theme.h) |
| §4.2 THEMES[7] catalog + theme_set frozen order + theme_active | Task 5 (data) + Task 7 (engine) |
| §4.3 7 display + body + mono fonts, subsets, manifest, freeze gate | Task 6 + Task 9.5 |
| §4.4 gauge_render bar/ring/cell/measure/bigfig + waveform/subdial deferred | Task 8 |
| §5.1 host unit tests (native env, table-driven) | Tasks 0-5 |
| §5.2 on-device DataStore smoke + theme-switch demo | Task 9 |
| §6 freeze gate (app fits ota_0) | Task 6.6 + Task 9.5 |
| FR-STATE-0 / FR-THEME-1/2/3/4 | Tasks 1-4 (contracts) / Tasks 5-9 (theme) |

### Native/device split (type consistency)

- `BEACON_NATIVE` fences every LVGL/Arduino include. Native compiles only: `records.h`, `screen_state.h`, `hublink.h`, `tickers.h`, `location.h`, `stale.h`, `ds_lock.h` (std::mutex branch), `datastore.{h,cpp}`, the LVGL-free parts of `theme.h`, and `theme_catalog.h`.
- Device compiles all of the above (FreeRTOS mutex branch) plus `theme.cpp`, `gauge.cpp`, `demo_screen.cpp`, fonts.
- `build_src_filter = +<core/datastore.cpp>` is the ONLY product `.cpp` linked into native; theme/gauge/demo `.cpp` never reach the host.
- `lv_color_t` is RGB565 (`LV_COLOR_DEPTH=16`, confirmed in lv_conf.h) — catalog stores `bt_rgb_t` and converts via `lv_color_make()` device-side, so no LVGL type crosses into native.

### Placeholder scan

- No `TBD`, no "similar to above", no pseudocode. The only literal "deferred"/"placeholder" tokens are the INTENTIONAL `GAUGE_WAVEFORM`/`GAUGE_SUBDIAL` deferred stubs (spec §4.4 explicitly defers them) and `<measure>`/`<sum>` blanks in the MANIFEST template (filled with real byte counts during Task 6/9).
- Every code step shows complete, compilable code (real LVGL 8.4 calls — `lv_arc_*`, `lv_obj_set_style_*`, `lv_label_*`, `xTaskCreatePinnedToCore`).

### Deviations from spec (additive seams, not contract changes)

- `DEFAULT_TICKERS_COUNT`, `WMO_MAP_COUNT` — derived size helpers the spec implies ("number of entries in DEFAULT_TICKERS").
- `ds_set_clock(ds_now_fn)` — injectable epoch clock so the LVGL/Arduino-free DataStore stamps `last_updated` deterministically (required for native testing). Internal seam, not a frozen-API change.
- `theme_on_rebuild()` + `theme_style_*()` accessors — the screen-rebuild hook + shared-style getters needed to make `theme_set`'s step 4 ("rebuild the active screen") work without the engine hardcoding a screen. Device-side only.
- `WMO_MAP` populated (spec left it as a `/* ... */` comment) with the standard Open-Meteo WMO buckets.
- Non-Editorial theme color hexes are concrete realizations of DESIGN.md's named accents (only Editorial has a full per-token hex table in DESIGN.md); ids/gauge/struct are frozen, hues are tunable on hardware in Task 9.

All frozen header code in Tasks 1, 2, 3 and the `beacon_theme_t`/`gauge_style_t` struct in Task 5 are copied VERBATIM from the spec; the only additions are explicitly-marked derived helpers and device-side engine seams.
