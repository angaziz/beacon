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
| `firmware/beacon/src/ui/gauge_style.h` | FROZEN: `gauge_style_t` enum (LVGL-free; shared by catalog + gauge) |
| `firmware/beacon/src/ui/theme.h` | `beacon_theme_t`, global space/motion `#define`s, theme API decls incl. the apply hook (spec §4.1/§4.2) |
| `firmware/beacon/src/ui/theme_catalog.h` | LVGL-free catalog data surface: ids + colors as host-shimmable `bt_rgb_t`; `THEME_COUNT` |
| `firmware/beacon/src/ui/theme.cpp` | `theme_set()` (updates tokens + calls apply hook), `theme_active()`, `theme_index()`, `THEMES[7]` (device-side) |
| `firmware/beacon/src/ui/gauge.h` | `gauge_render(parent, theme, pct)` decl |
| `firmware/beacon/src/ui/gauge.cpp` | `gauge_render()`: bar/ring/cell/measure/bigfig full; waveform/subdial deferred stub (device-side) |
| `firmware/beacon/src/ui/fonts/` | generated LVGL C-array font subsets (7 hero + 7 display + 5 body + 1 mono) + `MANIFEST.md` |
| `firmware/beacon/src/ui/demo_screen.{h,cpp}` | throwaway theme-switch demo (device-side) |
| `firmware/beacon/test/test_<unit>/test_main.cpp` | native-env Unity table-driven tests (flat: one folder per unit) |

---

## Task 0: native test env + Unity smoke test

Prove `pio test -e native` runs before writing any contract logic.

**Files:**
- Modify: `firmware/beacon/platformio.ini`
- Create: `firmware/beacon/test/test_smoke/test_main.cpp`

Before running host tests the first time, install the `native` platform once: `~/.beacon-pio/bin/pio platform install native`.

- [ ] **Step 0.1: Add `[env:native]` to platformio.ini (do NOT touch `[env:beacon]`)**

Append below the existing `[env:beacon]` block. PlatformIO's `native` platform builds host binaries with the system compiler; the bundled Unity framework is selected with `test_framework = unity`. The host tests live in flat `test/test_<unit>/` folders (each is its own program with its own `main()`); the native env runs them all (no `test_filter`), and `[env:beacon]` opts out of host tests entirely via `test_ignore = *` (Step 0.2).

```ini

[env:native]
; Host-only env for pure-logic unit tests (records / screen_state / datastore /
; config validation / theme catalog). NO Arduino, NO LVGL — kept out of native
; translation units via -DBEACON_NATIVE. Does not affect [env:beacon] above.
platform = native
test_framework = unity
test_build_src = yes   ; compile project src/ (filtered below) into test binaries, not just test/ files
; runs every test/test_<unit>/ folder (each is its own program with its own main()).
build_flags =
  -DBEACON_NATIVE=1        ; guards Arduino/LVGL-coupled code out of host builds
  -I src
  ; do NOT add -std=gnu++17 here: build_flags also hit Unity's C sources and clang
  ; rejects a C++ standard on .c files. Apple clang defaults to C++17 for .cpp (std::mutex OK).
; only datastore.cpp is LVGL/Arduino-free; everything else stays out of the host build
build_src_filter = -<*> +<core/datastore.cpp>
```

`-DBEACON_NATIVE=1` is the single guard symbol; product code uses `#ifndef BEACON_NATIVE` to fence off Arduino/LVGL includes. `test_build_src = yes` makes the filtered src/ compile into the test binaries. `build_src_filter` MUST stay a SINGLE line — an inline `;` comment breaks the multiline form — and compiles ONLY `datastore.cpp` into the host test binary (theme.cpp/gauge.cpp/demo_screen.cpp pull in LVGL and must never reach native).

- [ ] **Step 0.2: Keep the device env out of host tests**

Add this one line inside the existing `[env:beacon]` block (surgical — append after `monitor_filters`):

```ini
test_ignore = *   ; [env:beacon] runs no host unit tests; on-device checks use `pio run -t upload`
```

(This prevents `[env:beacon]` from ever picking up the `test/test_<unit>/` host folders. On-device checks in this plan run via `pio run -t upload`, not `pio test`.)

- [ ] **Step 0.3: Write the Unity smoke test (proves the harness runs)**

`firmware/beacon/test/test_smoke/test_main.cpp`:

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
git add firmware/beacon/platformio.ini firmware/beacon/test/test_smoke/test_main.cpp
```

Suggested message: `test(p0b): add native Unity env + smoke test`

---

## Task 1: FROZEN screen_state.h + records.h

Copy spec §3.1 + §3.2 VERBATIM. Test `record_age_s` (incl. `last_updated==0 => UINT32_MAX`) and that records compile on native.

**Files:**
- Create: `firmware/beacon/src/core/screen_state.h`
- Create: `firmware/beacon/src/core/records.h`
- Create: `firmware/beacon/test/test_records/test_main.cpp`

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

- [ ] **Step 1.3: Write the FAILING test FIRST (`test_records/test_main.cpp`)**

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

- [ ] **Step 1.4: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: both `test_records` tests PASS plus the prior smoke test (`3 Tests 0 Failures`).

- [ ] **Step 1.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/core/screen_state.h firmware/beacon/src/core/records.h firmware/beacon/test/test_records/test_main.cpp
```

Suggested message: `feat(p0b): freeze screen_state + records contracts (FR-STATE-0)`

---

## Task 2: config/tickers.h + config/location.h

VERBATIM from spec §3.5. Fill `WMO_MAP[]` (spec leaves it as a comment; populate the fixed table from DESIGN.md weather-condition needs). Table-test: ids unique, count <= MAX_TICKERS, enums in range.

**Files:**
- Create: `firmware/beacon/src/config/tickers.h`
- Create: `firmware/beacon/src/config/location.h`
- Create: `firmware/beacon/test/test_config/test_main.cpp`

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

- [ ] **Step 2.3: Write the FAILING test FIRST (`test_config/test_main.cpp`, table-driven)**

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

`test_config/test_main.cpp` includes `config/tickers.h` for `FIN_ID_LEN`? No — `FIN_ID_LEN` lives in `records.h`. Add `#include "core/records.h"` at the top of the test (the id-width assertion needs it). Make that include explicit.

- [ ] **Step 2.4: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: all config tests PASS.

- [ ] **Step 2.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/config/tickers.h firmware/beacon/src/config/location.h firmware/beacon/test/test_config/test_main.cpp
```

Suggested message: `feat(p0b): freeze ticker + location config schemas`

---

## Task 3: core/hublink.h FROZEN interface

VERBATIM from spec §3.4. It is an abstract interface (no impl until P2). Verify it is implementable via a trivial host mock.

**Files:**
- Create: `firmware/beacon/src/core/hublink.h`
- Create: `firmware/beacon/test/test_hublink/test_main.cpp`

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
git add firmware/beacon/src/core/hublink.h firmware/beacon/test/test_hublink/test_main.cpp
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
- Create: `firmware/beacon/test/test_datastore/test_main.cpp`

- [ ] **Step 4.1: Write `stale.h` (per-source staleness constants + finance lookup)**

Per-source `stale_s` come from `tech.md` §6 cadence table for the non-finance records; finance is per-ticker (`ticker_cfg_t.stale_s`). No magic numbers in the sweep — they live here.

```c
#pragma once
#include <stdint.h>
#include "config/tickers.h"

// Per-source stale thresholds (tech.md §6 cadence table). Finance is per-ticker.
#define WEATHER_STALE_S    1800u   // 30 min
#define USAGE_STALE_S       300u   // 5 min / hub-offline
#define BUDDY_STALE_S       300u
#define NOWPLAYING_STALE_S   15u

static inline uint32_t finance_stale_s(uint8_t idx) {
  return (idx < DEFAULT_TICKERS_COUNT) ? DEFAULT_TICKERS[idx].stale_s : 0u;
}
```

- [ ] **Step 4.2: Write `ds_lock.h` (lock wrapper — FreeRTOS on device, std::mutex on native)**

The DataStore must be host-testable, so the FreeRTOS mutex is abstracted. On native (`BEACON_NATIVE`) it is a `std::mutex`; on device it is a FreeRTOS recursive-free mutex. Critical sections are pure struct copies (`tech.md` §6), so a plain mutex is correct.

```c
#pragma once
// Thin mutex wrapper so datastore.cpp stays portable: std::mutex on the host test
// build (BEACON_NATIVE), a FreeRTOS mutex on device. Critical sections are pure
// struct copies (tech.md §6) — no I/O ever holds the lock.

#ifdef BEACON_NATIVE
  #include <mutex>
  typedef std::mutex ds_lock_t;
  static inline void ds_lock_init(ds_lock_t&) {}
  static inline void ds_lock_take(ds_lock_t& m) { m.lock(); }
  static inline void ds_lock_give(ds_lock_t& m) { m.unlock(); }
#else
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
  typedef SemaphoreHandle_t ds_lock_t;
  static inline void ds_lock_init(ds_lock_t& m) { m = xSemaphoreCreateMutex(); }
  static inline void ds_lock_take(ds_lock_t& m) { xSemaphoreTake(m, portMAX_DELAY); }
  static inline void ds_lock_give(ds_lock_t& m) { xSemaphoreGive(m); }
#endif
```

- [ ] **Step 4.3: Write `datastore.h` (spec §3.3 API + the symmetric explicit setters)**

The spec lists the core API and says "symmetric setters per domain (full list in records/datastore headers)". Provide explicit-state setters for every domain, plus the hub-offline + recovery setters.

```c
#pragma once
#include <stdint.h>
#include "core/records.h"

// FROZEN API (FR-STATE-0). Thread-safe per tech.md §6: Core-0 fetchers write, Core-1 UI
// reads by-value snapshots; the lock only guards struct copies. Setters force the record
// to ST_LIVE / ERR_NONE on success (clearing any prior error/hub-offline). Callers stamp
// hdr.last_updated (they know the fetch time); the staleness sweep consumes it.

void datastore_init(void);   // seeds finance_count + ids from DEFAULT_TICKERS, all ST_LOADING

// Setters (Core-0). Copy value fields; force hdr.state=ST_LIVE, hdr.err=ERR_NONE.
void ds_set_weather(const weather_rec_t* r);
void ds_set_finance(uint8_t idx, const finance_rec_t* r);  // preserves the slot's seeded id
void ds_set_usage(const usage_rec_t* r);
void ds_set_buddy(const buddy_rec_t* r);
void ds_set_nowplaying(const nowplaying_rec_t* r);

// Explicit failure/transport transitions (do NOT touch the value payload).
void ds_set_state_weather(screen_state_t s, data_err_t e);
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e);
void ds_set_state_nowplaying(screen_state_t s, data_err_t e);
void ds_set_hub_offline(void);   // flips usage + buddy to ST_HUB_OFFLINE

// Getters (Core-1). By-value snapshot under the lock.
weather_rec_t    ds_get_weather(void);
finance_rec_t    ds_get_finance(uint8_t idx);
uint8_t          ds_get_finance_count(void);
usage_rec_t      ds_get_usage(void);
buddy_rec_t      ds_get_buddy(void);
nowplaying_rec_t ds_get_nowplaying(void);

// Staleness sweep (~1/s from a Core-0 timer). For each record: if state==ST_LIVE and
// record_age_s(hdr, now) >= stale_s(source), promote to ST_STALE. Inclusive boundary.
// Never overwrites ST_OFFLINE / ST_ERROR / ST_HUB_OFFLINE (state-priority rule).
void ds_tick_staleness(uint32_t now);
```

(The DataStore carries NO clock: callers fill `hdr.last_updated` on the record they pass (they know the fetch time), and the sweep takes an explicit `now` — keeping the unit free of Arduino `time()`/RTC coupling and letting host tests drive time deterministically. Only weather/finance/nowplaying get explicit-state setters; usage/buddy transition via `ds_set_hub_offline`, since the BLE hub is their only transport.)

- [ ] **Step 4.4: Write `datastore.cpp` (LVGL/Arduino-free impl)**

```cpp
#include "core/datastore.h"
#include "core/ds_lock.h"
#include "core/stale.h"
#include "config/tickers.h"
#include <string.h>

static ds_lock_t        s_lock;
static weather_rec_t    s_weather;
static finance_rec_t    s_finance[MAX_TICKERS];
static uint8_t          s_finance_count;
static usage_rec_t      s_usage;
static buddy_rec_t      s_buddy;
static nowplaying_rec_t s_nowplaying;

static void hdr_loading(record_hdr_t* h) { h->last_updated = 0; h->state = ST_LOADING; h->err = ERR_NONE; }

void datastore_init(void) {
  ds_lock_init(s_lock);
  ds_lock_take(s_lock);
  memset(&s_weather, 0, sizeof(s_weather));       hdr_loading(&s_weather.hdr);
  memset(&s_usage, 0, sizeof(s_usage));           hdr_loading(&s_usage.hdr);
  memset(&s_buddy, 0, sizeof(s_buddy));           hdr_loading(&s_buddy.hdr);
  memset(&s_nowplaying, 0, sizeof(s_nowplaying));  hdr_loading(&s_nowplaying.hdr);

  s_finance_count = DEFAULT_TICKERS_COUNT;
  if (s_finance_count > MAX_TICKERS) s_finance_count = MAX_TICKERS;
  memset(s_finance, 0, sizeof(s_finance));
  for (uint8_t i = 0; i < s_finance_count; i++) {
    strncpy(s_finance[i].id, DEFAULT_TICKERS[i].id, FIN_ID_LEN - 1);
    s_finance[i].id[FIN_ID_LEN - 1] = 0;
    hdr_loading(&s_finance[i].hdr);
  }
  ds_lock_give(s_lock);
}

// --- setters: copy value, force LIVE/NONE (success clears prior error/hub-offline) ---
void ds_set_weather(const weather_rec_t* r) {
  ds_lock_take(s_lock);
  s_weather = *r; s_weather.hdr.state = ST_LIVE; s_weather.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_finance(uint8_t idx, const finance_rec_t* r) {
  if (idx >= MAX_TICKERS) return;
  ds_lock_take(s_lock);
  char id[FIN_ID_LEN]; strncpy(id, s_finance[idx].id, FIN_ID_LEN); id[FIN_ID_LEN - 1] = 0;
  s_finance[idx] = *r;
  strncpy(s_finance[idx].id, id, FIN_ID_LEN); s_finance[idx].id[FIN_ID_LEN - 1] = 0; // keep seeded id
  s_finance[idx].hdr.state = ST_LIVE; s_finance[idx].hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_usage(const usage_rec_t* r) {
  ds_lock_take(s_lock);
  s_usage = *r; s_usage.hdr.state = ST_LIVE; s_usage.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_buddy(const buddy_rec_t* r) {
  ds_lock_take(s_lock);
  s_buddy = *r; s_buddy.hdr.state = ST_LIVE; s_buddy.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}
void ds_set_nowplaying(const nowplaying_rec_t* r) {
  ds_lock_take(s_lock);
  s_nowplaying = *r; s_nowplaying.hdr.state = ST_LIVE; s_nowplaying.hdr.err = ERR_NONE;
  ds_lock_give(s_lock);
}

// --- explicit state transitions: do not touch value payload ---
void ds_set_state_weather(screen_state_t s, data_err_t e) {
  ds_lock_take(s_lock); s_weather.hdr.state = s; s_weather.hdr.err = e; ds_lock_give(s_lock);
}
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e) {
  if (idx >= MAX_TICKERS) return;
  ds_lock_take(s_lock); s_finance[idx].hdr.state = s; s_finance[idx].hdr.err = e; ds_lock_give(s_lock);
}
void ds_set_state_nowplaying(screen_state_t s, data_err_t e) {
  ds_lock_take(s_lock); s_nowplaying.hdr.state = s; s_nowplaying.hdr.err = e; ds_lock_give(s_lock);
}
void ds_set_hub_offline(void) {
  ds_lock_take(s_lock);
  s_usage.hdr.state = ST_HUB_OFFLINE;
  s_buddy.hdr.state = ST_HUB_OFFLINE;
  ds_lock_give(s_lock);
}

// --- getters: by-value snapshots ---
weather_rec_t ds_get_weather(void) {
  ds_lock_take(s_lock); weather_rec_t r = s_weather; ds_lock_give(s_lock); return r;
}
finance_rec_t ds_get_finance(uint8_t idx) {
  finance_rec_t r; memset(&r, 0, sizeof(r));
  if (idx >= MAX_TICKERS) { hdr_loading(&r.hdr); return r; }
  ds_lock_take(s_lock); r = s_finance[idx]; ds_lock_give(s_lock); return r;
}
uint8_t ds_get_finance_count(void) {
  ds_lock_take(s_lock); uint8_t c = s_finance_count; ds_lock_give(s_lock); return c;
}
usage_rec_t ds_get_usage(void) {
  ds_lock_take(s_lock); usage_rec_t r = s_usage; ds_lock_give(s_lock); return r;
}
buddy_rec_t ds_get_buddy(void) {
  ds_lock_take(s_lock); buddy_rec_t r = s_buddy; ds_lock_give(s_lock); return r;
}
nowplaying_rec_t ds_get_nowplaying(void) {
  ds_lock_take(s_lock); nowplaying_rec_t r = s_nowplaying; ds_lock_give(s_lock); return r;
}

// --- staleness sweep ---
static void sweep_one(record_hdr_t* h, uint32_t now, uint32_t stale_s) {
  if (h->state == ST_LIVE && record_age_s(h, now) >= stale_s) h->state = ST_STALE;
}
void ds_tick_staleness(uint32_t now) {
  ds_lock_take(s_lock);
  sweep_one(&s_weather.hdr,    now, WEATHER_STALE_S);
  sweep_one(&s_usage.hdr,      now, USAGE_STALE_S);
  sweep_one(&s_buddy.hdr,      now, BUDDY_STALE_S);
  sweep_one(&s_nowplaying.hdr, now, NOWPLAYING_STALE_S);
  for (uint8_t i = 0; i < s_finance_count; i++) sweep_one(&s_finance[i].hdr, now, finance_stale_s(i));
  ds_lock_give(s_lock);
}
```

- [ ] **Step 4.5: Write the FAILING tests FIRST (`test_datastore/test_main.cpp`, table-driven)**

```cpp
#include <unity.h>
#include <string.h>
#include "core/datastore.h"
#include "config/tickers.h"

void setUp(void) { datastore_init(); }   // reset the static store before each test
void tearDown(void) {}

static void test_init_seeds_finance(void) {
  TEST_ASSERT_EQUAL_UINT8(DEFAULT_TICKERS_COUNT, ds_get_finance_count());
  finance_rec_t f0 = ds_get_finance(0);
  TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[0].id, f0.id);
  TEST_ASSERT_EQUAL_INT(ST_LOADING, f0.hdr.state);
}

static void test_weather_set_get_forces_live(void) {
  weather_rec_t w; memset(&w, 0, sizeof(w));
  w.temp_c = 30.5f; w.humidity_pct = 70; w.wmo_code = 2;
  w.hdr.last_updated = 1000; w.hdr.state = ST_ERROR;   // setter must override to LIVE
  ds_set_weather(&w);
  weather_rec_t g = ds_get_weather();
  TEST_ASSERT_EQUAL_FLOAT(30.5f, g.temp_c);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, g.hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_NONE, g.hdr.err);
}

static void test_finance_isolation_and_id_preserved(void) {
  finance_rec_t r; memset(&r, 0, sizeof(r));
  r.value = 123.0; r.hdr.last_updated = 1000;
  strcpy(r.id, "WRONG");                  // caller id ignored; slot keeps seeded id
  ds_set_finance(2, &r);
  finance_rec_t s2 = ds_get_finance(2);
  finance_rec_t s3 = ds_get_finance(3);
  TEST_ASSERT_EQUAL_FLOAT(123.0f, (float)s2.value);
  TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[2].id, s2.id);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, s2.hdr.state);
  TEST_ASSERT_EQUAL_INT(ST_LOADING, s3.hdr.state);   // neighbor untouched
}

static void test_staleness_inclusive_boundary(void) {
  struct { uint32_t last; uint32_t now; int expect; } cases[] = {
    {1000, 1000 + 1799, ST_LIVE},
    {1000, 1000 + 1800, ST_STALE},   // WEATHER_STALE_S, inclusive
    {1000, 1000 + 5000, ST_STALE},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    datastore_init();
    weather_rec_t w; memset(&w, 0, sizeof(w)); w.hdr.last_updated = cases[i].last;
    ds_set_weather(&w);                // LIVE
    ds_tick_staleness(cases[i].now);
    TEST_ASSERT_EQUAL_INT(cases[i].expect, ds_get_weather().hdr.state);
  }
}

static void test_sweep_never_clobbers_explicit_states(void) {
  ds_set_state_weather(ST_ERROR, ERR_RATE_LIMITED);
  ds_tick_staleness(9999999);
  TEST_ASSERT_EQUAL_INT(ST_ERROR, ds_get_weather().hdr.state);   // not promoted to STALE
  ds_set_hub_offline();
  ds_tick_staleness(9999999);
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_usage().hdr.state);
}

static void test_hub_offline_flip_and_recovery(void) {
  ds_set_hub_offline();
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_usage().hdr.state);
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_buddy().hdr.state);
  usage_rec_t u; memset(&u, 0, sizeof(u)); u.claude.h5.pct = 24; u.hdr.last_updated = 1000;
  ds_set_usage(&u);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, ds_get_usage().hdr.state);      // recovered
  TEST_ASSERT_EQUAL_INT16(24, ds_get_usage().claude.h5.pct);
}

static void test_explicit_failure_preserves_payload(void) {
  weather_rec_t w; memset(&w, 0, sizeof(w)); w.temp_c = 28.0f; w.hdr.last_updated = 1000;
  ds_set_weather(&w);
  ds_set_state_weather(ST_OFFLINE, ERR_NO_ROUTE);
  weather_rec_t g = ds_get_weather();
  TEST_ASSERT_EQUAL_INT(ST_OFFLINE, g.hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_NO_ROUTE, g.hdr.err);
  TEST_ASSERT_EQUAL_FLOAT(28.0f, g.temp_c);                      // value preserved
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_seeds_finance);
  RUN_TEST(test_weather_set_get_forces_live);
  RUN_TEST(test_finance_isolation_and_id_preserved);
  RUN_TEST(test_staleness_inclusive_boundary);
  RUN_TEST(test_sweep_never_clobbers_explicit_states);
  RUN_TEST(test_hub_offline_flip_and_recovery);
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
git add firmware/beacon/src/core/stale.h firmware/beacon/src/core/ds_lock.h firmware/beacon/src/core/datastore.h firmware/beacon/src/core/datastore.cpp firmware/beacon/test/test_datastore/test_main.cpp
```

Suggested message: `feat(p0b): thread-safe DataStore with staleness sweep + state priority`

---

## Task 5: ui/gauge_style.h + theme.h + theme_catalog.h (catalog data + host lookup test)

`beacon_theme_t` (spec §4.1, with `f_hero` + `radius`/`stroke_hair`/`stroke_med` added vs tech.md §6) + `gauge_style_t` + global space/motion `#define`s + the catalog. `lv_color_t`/`lv_font_t` make a full native compile hard, so split: `gauge_style_t` lives in its own LVGL-free `gauge_style.h`; a LVGL-free `theme_catalog.h` carries ids + colors as a host-shimmable `bt_rgb_t`; the `beacon_theme_t` struct (with `lv_color_t` + `const lv_font_t*`) and `THEMES[7]` building stay in `theme.h`/`theme.cpp` (device-side). The host theme test includes ONLY `theme_catalog.h` (which needs only `gauge_style.h`), never `theme.h`.

**Split rationale:**
- **LVGL-free, host-testable:** `gauge_style.h` (the enum) and `theme_catalog.h` (the 7 theme `id` strings, gauge-style per theme, color tokens as `bt_rgb_t {r,g,b}`, and the per-theme `glow`/`radius`/`stroke_*` bytes). Pure data carrying the DESIGN.md values.
- **Device-side only:** `theme.h`'s `beacon_theme_t` + the runtime `THEMES[7]` (which hold `lv_color_t` + `const lv_font_t*`), built in `theme.cpp` by converting each `bt_rgb_t` via `lv_color_make()` and pointing at the generated fonts (Task 6). `theme.h` includes `<lvgl.h>` unconditionally — it never reaches a native translation unit because no native test includes it.

**Files:**
- Create: `firmware/beacon/src/ui/gauge_style.h`
- Create: `firmware/beacon/src/ui/theme.h`
- Create: `firmware/beacon/src/ui/theme_catalog.h`
- Create: `firmware/beacon/test/test_theme/test_main.cpp`

- [ ] **Step 5.1a: Write `gauge_style.h` (LVGL-free enum, shared by catalog + gauge)**

```c
#pragma once
// LVGL-free so both the host-testable theme catalog and the device theme/gauge code share it.
typedef enum {
  GAUGE_BAR, GAUGE_RING, GAUGE_CELL, GAUGE_WAVEFORM, GAUGE_MEASURE, GAUGE_BIGFIG, GAUGE_SUBDIAL
} gauge_style_t;
```

- [ ] **Step 5.1b: Write `theme.h` (spec §4.1 struct + §4.2 apply-hook API + global tokens)**

`theme.h` is device-side (includes `<lvgl.h>`); it is never compiled into a native translation unit. The struct field order is copied VERBATIM from the spec: `f_hero` first, then `f_display, f_body, f_mono`, then `gauge`, `glow`, `radius`, `stroke_hair, stroke_med`.

```c
#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "ui/gauge_style.h"

// FROZEN runtime theme contract (tech.md §6 + DESIGN.md tokens). Screens read tokens only.
// Built at runtime from THEME_CATALOG (theme_catalog.h) + the resident fonts (theme.cpp).
typedef struct {
  const char*      id;
  lv_color_t       bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  const lv_font_t *f_hero;      // oversized figures (clock, big %): digits + :%°.,+-/ subset
  const lv_font_t *f_display, *f_body, *f_mono; // display=titles/section figures (full ASCII); body/mono per role
  gauge_style_t    gauge;
  uint8_t          glow;
  uint8_t          radius;
  uint8_t          stroke_hair, stroke_med;
} beacon_theme_t;

// Global (not per-theme) tokens, DESIGN.md.
#define SPACE_XS   4
#define SPACE_S    8
#define SPACE_M   12
#define SPACE_L   16
#define SPACE_XL  24
#define SPACE_XXL 32
#define DUR_FAST  120   // ms
#define DUR       220
#define DUR_SLOW  400
// Easing is a single shared ease-out (no per-theme easing). Reduced-motion is a chunk-D
// Settings flag the motion helpers consult to crossfade/instant instead of animate.

// The active screen registers a rebuild hook; theme_set() invokes it after updating tokens so the
// screen tears down + rebuilds from the new theme (no reboot, FR-THEME-3). The screen's hook is
// responsible for the frozen teardown order: clear objects + remove styles, reset its lv_style_t,
// rebuild from the passed tokens (one-theme-resident, tech.md §6).
typedef void (*theme_apply_cb)(const beacon_theme_t*);
void theme_on_apply(theme_apply_cb cb);

void theme_set(uint8_t idx);                 // idx < THEME_COUNT; updates tokens + calls the apply hook
const beacon_theme_t* theme_active(void);    // current theme tokens (NULL before first theme_set)
uint8_t theme_index(void);                   // current index
```

(`THEME_COUNT` is defined in `theme_catalog.h`, not here — the catalog owns the array size. The space-token names are `SPACE_XS/S/M/L/XL/XXL`.)

- [ ] **Step 5.2: Write `theme_catalog.h` (LVGL-free catalog data with DESIGN.md values)**

Colors are the DESIGN.md token values (Editorial default table + catalog realizations), expressed as decimal `bt_rgb_t` triples so the catalog is host-testable; `theme.cpp` converts to `lv_color_t`. The `line` token in DESIGN is a translucent white (`rgba(255,255,255,.14)`) — flattened on pure-black canvas to its opaque equivalent `{36,36,34}`. `theme_catalog.h` includes `ui/gauge_style.h` (the enum) only — NOT `theme.h`.

```c
#pragma once
#include <stdint.h>
#include "ui/gauge_style.h"

// LVGL-free catalog data (host-testable). theme.cpp maps this -> beacon_theme_t (lv_color_t/lv_font_t).
// DESIGN.md owns token VALUES; only Editorial has full hex there, so the other six are concrete
// realizations of DESIGN.md's named accents (tunable on hardware in the Task 9 demo). bg is always
// pure black (AMOLED off-pixels). The ids + gauge mapping + struct are the frozen part.

typedef struct { uint8_t r, g, b; } bt_rgb_t;

typedef struct {
  const char*   id;                 // canonical id (DESIGN.md)
  bt_rgb_t      bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  gauge_style_t gauge;
  uint8_t       glow;               // 0..255
  uint8_t       radius;             // element corner radius (px)
  uint8_t       stroke_hair, stroke_med;
} theme_catalog_t;

#define THEME_COUNT 7

static const theme_catalog_t THEME_CATALOG[THEME_COUNT] = {
  // Editorial Index (default) — DESIGN.md exact values
  { "editorial",
    {0,0,0}, {244,243,239}, {116,114,108}, {36,36,34}, {255,74,43}, {255,74,43},
    {244,243,239}, {255,74,43}, {255,74,43}, GAUGE_BAR, 0, 0, 1, 2 },
  // Aerospace HUD — cyan + amber, concentric rings
  { "hud",
    {0,0,0}, {224,247,250}, {96,125,139}, {20,40,46}, {0,229,255}, {255,179,0},
    {0,229,255}, {255,82,82}, {255,179,0}, GAUGE_RING, 40, 2, 1, 2 },
  // Calm Futurism — faint red, sparse white-on-black, big figures
  { "calm",
    {0,0,0}, {238,238,238}, {120,120,120}, {30,30,30}, {224,86,74}, {224,86,74},
    {238,238,238}, {224,86,74}, {224,86,74}, GAUGE_BIGFIG, 10, 8, 1, 2 },
  // Blueprint — draftsman blue, dimension lines
  { "blueprint",
    {0,0,0}, {214,230,245}, {90,120,150}, {26,42,58}, {74,144,217}, {74,144,217},
    {120,200,140}, {230,120,110}, {74,144,217}, GAUGE_MEASURE, 0, 0, 1, 2 },
  // LED Matrix — amber lit-pixel
  { "led",
    {0,0,0}, {255,176,0}, {120,82,0}, {40,28,0}, {255,176,0}, {255,176,0},
    {255,176,0}, {255,80,40}, {255,80,40}, GAUGE_CELL, 60, 0, 1, 2 },
  // Oscilloscope — phosphor green graticule + trace
  { "oscilloscope",
    {0,0,0}, {51,255,102}, {28,110,56}, {16,48,24}, {51,255,102}, {51,255,102},
    {51,255,102}, {255,90,90}, {255,210,80}, GAUGE_WAVEFORM, 50, 0, 1, 2 },
  // Analog Neo — ice blue, analog hands + sub-dials
  { "analog",
    {0,0,0}, {191,227,242}, {96,128,144}, {28,40,48}, {159,208,232}, {159,208,232},
    {191,227,242}, {232,140,140}, {159,208,232}, GAUGE_SUBDIAL, 20, 12, 1, 2 },
};
```

(Color values for the non-default themes are concrete realizations of DESIGN.md's named accents — "cyan + amber", "phosphor green", "ice blue", etc. — since DESIGN.md gives full per-token hex only for Editorial. Adjust on hardware in Task 9 if a hue reads wrong, but the ids/gauge/struct shape are frozen.)

- [ ] **Step 5.3: Write the FAILING test FIRST (`test_theme/test_main.cpp`, table-driven)**

The host test includes ONLY `theme_catalog.h` (LVGL-free) — never `theme.h`.

```cpp
#include <unity.h>
#include <string.h>
#include "ui/theme_catalog.h"   // LVGL-free; the device beacon_theme_t (theme.h) wraps this

void setUp(void) {}
void tearDown(void) {}

static void test_catalog_count(void) {
  TEST_ASSERT_EQUAL_INT(7, THEME_COUNT);
  TEST_ASSERT_EQUAL_INT(THEME_COUNT, (int)(sizeof(THEME_CATALOG) / sizeof(THEME_CATALOG[0])));
}

// ids match the DESIGN.md canonical set, in order, unique. default at index 0.
static void test_catalog_ids(void) {
  const char* expect[THEME_COUNT] = {
    "editorial", "hud", "calm", "blueprint", "led", "oscilloscope", "analog"
  };
  for (int i = 0; i < THEME_COUNT; i++) {
    TEST_ASSERT_NOT_NULL(THEME_CATALOG[i].id);
    TEST_ASSERT_EQUAL_STRING(expect[i], THEME_CATALOG[i].id);
    for (int j = i + 1; j < THEME_COUNT; j++)
      TEST_ASSERT_FALSE(strcmp(THEME_CATALOG[i].id, THEME_CATALOG[j].id) == 0);
  }
}

// each entry's gauge is a valid gauge_style_t and bg is pure black (DESIGN: black is free).
static void test_catalog_invariants(void) {
  for (int i = 0; i < THEME_COUNT; i++) {
    const theme_catalog_t* t = &THEME_CATALOG[i];
    TEST_ASSERT_TRUE_MESSAGE(t->gauge <= GAUGE_SUBDIAL, "gauge out of range");
    TEST_ASSERT_EQUAL_UINT8(0, t->bg.r);
    TEST_ASSERT_EQUAL_UINT8(0, t->bg.g);
    TEST_ASSERT_EQUAL_UINT8(0, t->bg.b);
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
  RUN_TEST(test_catalog_ids);
  RUN_TEST(test_catalog_invariants);
  RUN_TEST(test_catalog_gauge_mapping);
  return UNITY_END();
}
```

- [ ] **Step 5.4: Verify (host)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`. Expected: all theme-catalog tests PASS. The host test compiles because it includes only `theme_catalog.h` (which includes only `gauge_style.h`); `theme.h`'s `lv_color_t`/`lv_font_t` struct never enters the native build.

- [ ] **Step 5.5: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/gauge_style.h firmware/beacon/src/ui/theme.h firmware/beacon/src/ui/theme_catalog.h firmware/beacon/test/test_theme/test_main.cpp
```

Suggested message: `feat(p0b): theme struct + global tokens + host-testable catalog`

---

## Task 6: Fonts — source, subset, generate, budget (FREEZE GATE)

Source 7 typefaces, instance the variable ones to static weights, glyph-subset into LVGL C-array fonts via `npx lv_font_conv@1.5.3` (compressed), commit under `ui/fonts/`, produce the manifest, and confirm the app fits the 3MB `ota_0` slot. Device/tooling task — no Unity; verification is a build size check.

**Font roles (spec §4.3) => 20 generated arrays:** each typeface ships a hero subset (84px, digits + `:%.,+-/° ` symbols only, for clock / big %) and a display subset (30px, full printable ASCII + `°`, for titles / now-playing track / section figures); 5 families also ship a body subset (18px, full ASCII + `°`); JetBrains Mono ships the single shared mono subset (15px, full ASCII + `°`, used by every theme). Total = 7 hero + 7 display + 5 body + 1 mono = 20 C arrays.

| Theme | hero / display | body | mono |
|---|---|---|---|
| editorial | Space Grotesk | Space Grotesk | JetBrains Mono |
| hud | Rajdhani | Rajdhani | JetBrains Mono |
| calm | Doto | Inter | JetBrains Mono |
| blueprint | Chakra Petch | Chakra Petch | JetBrains Mono |
| led | Pixelify Sans | Inter | JetBrains Mono |
| oscilloscope | JetBrains Mono | JetBrains Mono | JetBrains Mono |
| analog | Inter | Inter | JetBrains Mono |

(calm/led/analog have no own body subset — they reuse Inter's body; oscilloscope reuses JetBrains Mono's body. That is why only 5 body arrays exist: Space Grotesk, Rajdhani, Chakra Petch, Inter, JetBrains Mono.)

**Files:**
- Create: `firmware/beacon/src/ui/fonts/font_*.c` (20 generated arrays)
- Create: `firmware/beacon/src/ui/fonts/fonts.h` (`extern const lv_font_t` decls for all subsets)
- Create: `firmware/beacon/src/ui/fonts/MANIFEST.md` (sources + roles + the asset/flash budget)
- Modify: `firmware/beacon/src/lv_conf.h` (set `LV_USE_FONT_COMPRESSED 1` — the generated fonts are compressed)

- [ ] **Step 6.1: Get the TTFs (instance to static weights in a scratch dir; do NOT commit TTFs)**

Pull the 7 typefaces from the `google/fonts` repo raw paths (the `floriankarsten` paths 404'd — use `google/fonts`). Variable fonts are instanced to static weights via `fonttools varLib.instancer`.

```
| Family | google/fonts path | Instanced weight |
|---|---|---|
| Space Grotesk | ofl/spacegrotesk/SpaceGrotesk[wght].ttf | wght=500 |
| Rajdhani | ofl/rajdhani/Rajdhani-Medium.ttf | static Medium |
| Doto | ofl/doto/Doto[ROND,wght].ttf | wght=500, ROND=0 |
| Chakra Petch | ofl/chakrapetch/ChakraPetch-Medium.ttf | static Medium |
| Pixelify Sans | ofl/pixelifysans/PixelifySans[wght].ttf | wght=400 |
| JetBrains Mono | ofl/jetbrainsmono/JetBrainsMono[wght].ttf | wght=500 |
| Inter | ofl/inter/Inter[opsz,wght].ttf | wght=400, opsz=14 |
```

```bash
mkdir -p /tmp/beaconfonts/ttf && cd /tmp/beaconfonts
# fetch each from https://raw.githubusercontent.com/google/fonts/main/<path>, then for the
# variable ones instance to a static weight, e.g.:
#   fonttools varLib.instancer SpaceGrotesk[wght].ttf wght=500 -o ttf/SpaceGrotesk.ttf
#   fonttools varLib.instancer "Doto[ROND,wght].ttf" wght=500 ROND=0 -o ttf/Doto.ttf
# the two already-static Medium files are copied straight into ttf/.
```

(Fonts are OFL — license permits embedding. Keep TTFs out of the repo; only the generated `.c` arrays are committed.)

- [ ] **Step 6.2: Generate the LVGL C-array subsets with `npx lv_font_conv@1.5.3` (compressed)**

Two glyph profiles. `-r 0x20-0x7E -r 0xB0` is full printable ASCII + `°`; the hero subset uses `--symbols "0123456789:%.,+-/° "`. `--bpp 4` throughout; `--format lvgl`. The generated fonts are COMPRESSED (`bitmap_format=1`), so `LV_USE_FONT_COMPRESSED` must be 1 (Step 6.4) or all text renders blank.

NOTE: zsh does not word-split an unquoted `$VAR`, so the per-family loop wraps the call in a helper function rather than relying on word-splitting:

```bash
cd /tmp/beaconfonts
OUT="$(git rev-parse --show-toplevel)/firmware/beacon/src/ui/fonts"
HERO_SYMS="0123456789:%.,+-/° "

gen() {  # gen <ttf> <size> <key> <role> [hero]
  local ttf="$1" size="$2" key="$3" role="$4" hero="$5"
  if [ "$hero" = "hero" ]; then
    npx -y lv_font_conv@1.5.3 --font "ttf/$ttf" --size "$size" --bpp 4 --format lvgl \
      --symbols "$HERO_SYMS" -o "$OUT/font_${key}_${role}.c"
  else
    npx -y lv_font_conv@1.5.3 --font "ttf/$ttf" --size "$size" --bpp 4 --format lvgl \
      -r 0x20-0x7E -r 0xB0 -o "$OUT/font_${key}_${role}.c"
  fi
}

# hero (84) + display (30) per family; body (18) for the 5 families that own one; mono (15) once.
gen SpaceGrotesk.ttf 84 sg    hero hero;  gen SpaceGrotesk.ttf 30 sg    disp; gen SpaceGrotesk.ttf 18 sg    body
gen Rajdhani.ttf     84 raj   hero hero;  gen Rajdhani.ttf     30 raj   disp; gen Rajdhani.ttf     18 raj   body
gen Doto.ttf         84 doto  hero hero;  gen Doto.ttf         30 doto  disp
gen ChakraPetch.ttf  84 cp    hero hero;  gen ChakraPetch.ttf  30 cp    disp; gen ChakraPetch.ttf  18 cp    body
gen PixelifySans.ttf 84 pix   hero hero;  gen PixelifySans.ttf 30 pix   disp
gen JetBrainsMono.ttf 84 jbm  hero hero;  gen JetBrainsMono.ttf 30 jbm  disp; gen JetBrainsMono.ttf 18 jbm  body
gen Inter.ttf        84 inter hero hero;  gen Inter.ttf        30 inter disp; gen Inter.ttf        18 inter body
gen JetBrainsMono.ttf 15 jbm  mono
```

(The variable names == generated filenames: `font_sg_hero`, `font_sg_disp`, `font_sg_body`, ... `font_jbm_mono`. `THEME_FONTS[]` in `theme.cpp` (Task 7) maps each theme to its hero/disp/body/mono per the table above.)

- [ ] **Step 6.3: Write `fonts.h` (declare every generated subset)**

The generated arrays are plain `const lv_font_t` objects, declared `extern`:

```c
#pragma once
#include <lvgl.h>

// Glyph-subset fonts generated by lv_font_conv (see MANIFEST.md).
// Variable names == generated filenames. hero = 84px digits+symbols; disp = 30px full ASCII;
// body = 18px full ASCII; mono = 15px full ASCII (JetBrains Mono, shared by all themes).
extern const lv_font_t font_sg_hero,    font_sg_disp,    font_sg_body;     // Space Grotesk
extern const lv_font_t font_raj_hero,   font_raj_disp,   font_raj_body;    // Rajdhani
extern const lv_font_t font_doto_hero,  font_doto_disp;                    // Doto
extern const lv_font_t font_cp_hero,    font_cp_disp,    font_cp_body;     // Chakra Petch
extern const lv_font_t font_pix_hero,   font_pix_disp;                     // Pixelify Sans
extern const lv_font_t font_jbm_hero,   font_jbm_disp,   font_jbm_body, font_jbm_mono; // JetBrains Mono
extern const lv_font_t font_inter_hero, font_inter_disp, font_inter_body;  // Inter
```

- [ ] **Step 6.4: Enable compressed-font decoding in lv_conf.h**

The generated fonts are compressed (`bitmap_format=1`). Set the existing flag to 1 (with it at 0, gauge shapes still draw but ALL text renders blank):

```c
#define LV_USE_FONT_COMPRESSED 1   /*lv_font_conv emits compressed glyphs (.bitmap_format=1); LVGL must decode them*/
```

(No `LV_FONT_CUSTOM_DECLARE` needed — the fonts are referenced directly through `fonts.h`'s `extern` decls from `theme.cpp`, not registered with LVGL's built-in font subsystem.)

- [ ] **Step 6.5: Write `MANIFEST.md` (sources + roles + the asset/flash budget)**

Create `firmware/beacon/src/ui/fonts/MANIFEST.md` documenting: the `google/fonts` source paths + instanced weights (Step 6.1 table), the role/size/glyph table (hero 84 / display 30 / body 18 / mono 15), the per-theme `THEME_FONTS[]` mapping, the regenerate recipe, and the budget. Generated C totals ~786 KB of the 3 MB app slot (GC-dropped until referenced; real linked cost is measured by the Task 9 demo build). Record the measured `.bin` Flash% as the freeze-gate evidence.

- [ ] **Step 6.6: Verify (FREEZE GATE — device build size)** — install the native platform once if not already (`~/.beacon-pio/bin/pio platform install native`), then build and read the Flash summary:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon
```

(Note: fonts only link in once `theme.cpp` from Task 7 references them via `THEME_FONTS[]`; if the linker drops unused arrays, defer the size gate to Task 9's build where the demo references all 7 themes.) Expected: build succeeds; PlatformIO prints `Flash: [== ] xx.x% (used N bytes from 3145728 bytes)`. **GATE: the used bytes must be < 3145728 (ota_0) with comfortable margin** (fonts ~786 KB). Record N in MANIFEST.md and confirm PASS.

- [ ] **Step 6.7: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/fonts firmware/beacon/src/lv_conf.h
```

Suggested message: `feat(p0b): glyph-subset theme fonts + flash-budget manifest`

---

## Task 7: ui/theme.cpp — theme_set() (apply-hook model) + theme_active()/theme_index()

Build the runtime `THEMES[7]` from `THEME_CATALOG` + the fonts (mapping `bt_rgb_t` -> `lv_color_t` and pointing at the generated fonts via `THEME_FONTS[]`); implement `theme_set()` (update tokens, then call the apply hook), `theme_active()`, and `theme_index()`. The teardown/rebuild itself lives in the screen's apply hook (Task 9), NOT in the engine. Device task.

**Files:**
- Create: `firmware/beacon/src/ui/theme.cpp`

- [ ] **Step 7.1: Write `theme.cpp`**

`theme_set(idx)` maps the catalog entry into the resident `s_theme` token struct (fonts from `THEME_FONTS[idx]`), then invokes the registered apply hook so the active screen tears down + rebuilds from the new tokens. The engine holds no `lv_style_t` set and never touches the screen tree — the screen owns its teardown (one-theme-resident). `THEME_FONTS[]` selects each theme's `{hero, disp, body, mono}` per the MANIFEST mapping (mono is `font_jbm_mono`, shared by all).

```cpp
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/fonts/fonts.h"

// Per-theme font selection (MANIFEST.md). mono is shared (JetBrains Mono) across all themes.
typedef struct { const lv_font_t *hero, *disp, *body, *mono; } theme_fonts_t;
static const theme_fonts_t THEME_FONTS[THEME_COUNT] = {
  {&font_sg_hero,    &font_sg_disp,    &font_sg_body,    &font_jbm_mono},  // editorial
  {&font_raj_hero,   &font_raj_disp,   &font_raj_body,   &font_jbm_mono},  // hud
  {&font_doto_hero,  &font_doto_disp,  &font_inter_body, &font_jbm_mono},  // calm
  {&font_cp_hero,    &font_cp_disp,    &font_cp_body,    &font_jbm_mono},  // blueprint
  {&font_pix_hero,   &font_pix_disp,   &font_inter_body, &font_jbm_mono},  // led
  {&font_jbm_hero,   &font_jbm_disp,   &font_jbm_body,   &font_jbm_mono},  // oscilloscope
  {&font_inter_hero, &font_inter_disp, &font_inter_body, &font_jbm_mono},  // analog
};

static beacon_theme_t s_theme;
static uint8_t        s_idx = 0;
static theme_apply_cb s_apply = nullptr;

static inline lv_color_t C(bt_rgb_t c) { return lv_color_make(c.r, c.g, c.b); }

void theme_on_apply(theme_apply_cb cb) { s_apply = cb; }

void theme_set(uint8_t idx) {
  if (idx >= THEME_COUNT) return;
  s_idx = idx;
  const theme_catalog_t* t = &THEME_CATALOG[idx];
  const theme_fonts_t*   f = &THEME_FONTS[idx];

  s_theme.id      = t->id;
  s_theme.bg      = C(t->bg);
  s_theme.ink     = C(t->ink);
  s_theme.ink_dim = C(t->ink_dim);
  s_theme.line    = C(t->line);
  s_theme.accent  = C(t->accent);
  s_theme.accent2 = C(t->accent2);
  s_theme.up      = C(t->up);
  s_theme.down    = C(t->down);
  s_theme.alert   = C(t->alert);
  s_theme.f_hero    = f->hero;
  s_theme.f_display = f->disp;
  s_theme.f_body    = f->body;
  s_theme.f_mono    = f->mono;
  s_theme.gauge       = t->gauge;
  s_theme.glow        = t->glow;
  s_theme.radius      = t->radius;
  s_theme.stroke_hair = t->stroke_hair;
  s_theme.stroke_med  = t->stroke_med;

  if (s_apply) s_apply(&s_theme);   // screen tears down + rebuilds from the new tokens
}

const beacon_theme_t* theme_active(void) { return s_theme.id ? &s_theme : nullptr; }
uint8_t theme_index(void) { return s_idx; }
```

(All of `theme_on_apply`, `theme_set`, `theme_active`, `theme_index`, and the `theme_apply_cb` typedef are declared in `theme.h` (Task 5) — no extra decls needed here.)

- [ ] **Step 7.2: Verify (device build)** — `cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon`. Expected: links clean (theme.cpp + fonts compile into the image). No runtime check yet — that is Task 9.

- [ ] **Step 7.3: Verify native is untouched** — `~/.beacon-pio/bin/pio test -e native`. Expected: still all PASS (theme.cpp is NOT in `build_src_filter` beyond datastore.cpp, so native ignores it).

- [ ] **Step 7.4: Commit checkpoint** —

```bash
git add firmware/beacon/src/ui/theme.cpp firmware/beacon/src/ui/theme.h
```

Suggested message: `feat(p0b): theme engine - theme_set updates tokens then calls the apply hook (FR-THEME-1/3)`

---

## Task 8: ui/gauge.{h,cpp} — gauge_render() (bar/ring/cell/measure/bigfig; waveform/subdial deferred)

`gauge_render(parent, th, pct)` renders a 0..100 level into a fixed-size root using the PASSED theme's `gauge` style (the theme is passed in explicitly, not read from a global). bar/ring/cell/measure/bigfig are built from stock LVGL 8.4 widgets; waveform (oscilloscope) + subdial (analog) are bespoke widgets deferred to their screens and render a labeled placeholder. Device task.

**Files:**
- Create: `firmware/beacon/src/ui/gauge.h`
- Create: `firmware/beacon/src/ui/gauge.cpp`

- [ ] **Step 8.1: Write `gauge.h`**

```c
#pragma once
#include <lvgl.h>
#include "ui/theme.h"

// Render a 0..100 level into `parent` using the theme's gauge style. Returns the created
// root object (caller positions/sizes it). bar/ring/cell/measure/bigfig are implemented;
// waveform (oscilloscope) + subdial (analog) are bespoke widgets deferred to their screens
// and render a labeled placeholder for now (DESIGN.md outlier widgets).
lv_obj_t* gauge_render(lv_obj_t* parent, const beacon_theme_t* th, uint8_t pct);
```

- [ ] **Step 8.2: Write `gauge.cpp` (bar/ring/cell/measure/bigfig full; waveform/subdial stub)**

Each style draws into a fixed `GAUGE_W x GAUGE_H` transparent root (`make_root`). `pct` is clamped to 100. bar/measure use `lv_bar`, ring uses `lv_arc`, cell lays out `CELL_COUNT` segments via flex, bigfig prints the value in the theme's hero font, and the two bespoke styles render a labeled placeholder.

```cpp
#include "ui/gauge.h"
#include <stdio.h>

#define GAUGE_W 220
#define GAUGE_H 120
#define CELL_COUNT 10

static lv_obj_t* make_root(lv_obj_t* parent) {
  lv_obj_t* g = lv_obj_create(parent);
  lv_obj_set_size(g, GAUGE_W, GAUGE_H);
  lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g, 0, 0);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  return g;
}

static void gauge_bar(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  lv_obj_t* bar = lv_bar_create(g);
  lv_obj_set_size(bar, GAUGE_W, 14);
  lv_obj_center(bar);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_radius(bar, th->radius, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, th->radius, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, th->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, th->accent, LV_PART_INDICATOR);
}

static void gauge_ring(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  lv_obj_t* arc = lv_arc_create(g);
  lv_obj_set_size(arc, GAUGE_H - 8, GAUGE_H - 8);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, pct);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, th->line, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, th->accent, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
}

static void gauge_cell(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  uint8_t lit = (uint8_t)((pct + 5) / 10);
  for (int i = 0; i < CELL_COUNT; i++) {
    lv_obj_t* c = lv_obj_create(g);
    lv_obj_set_size(c, 14, 36);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_color(c, (i < lit) ? th->accent : th->line, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  }
}

static void gauge_measure(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  // draftsman dimension: a flat bar with end-cap ticks + a numeric label
  lv_obj_t* bar = lv_bar_create(g);
  lv_obj_set_size(bar, GAUGE_W, 6);
  lv_obj_align(bar, LV_ALIGN_CENTER, 0, 8);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, th->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, th->accent, LV_PART_INDICATOR);
  for (int i = 0; i < 3; i++) {  // ticks at 0/50/100
    lv_obj_t* tick = lv_obj_create(g);
    lv_obj_set_size(tick, th->stroke_med, 14);
    lv_obj_set_style_bg_color(tick, th->line, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_align(tick, LV_ALIGN_CENTER, (int16_t)(-GAUGE_W / 2 + i * (GAUGE_W / 2)), 8);
  }
  char buf[8]; snprintf(buf, sizeof(buf), "%u%%", pct);
  lv_obj_t* lbl = lv_label_create(g);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, th->ink, 0);
  lv_obj_set_style_text_font(lbl, th->f_mono, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -22);
}

static void gauge_bigfig(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  char buf[8]; snprintf(buf, sizeof(buf), "%u%%", pct);
  lv_obj_t* lbl = lv_label_create(g);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, th->accent, 0);
  lv_obj_set_style_text_font(lbl, th->f_hero, 0);
  lv_obj_center(lbl);
}

static void gauge_stub(lv_obj_t* g, const beacon_theme_t* th, const char* what) {
  lv_obj_t* lbl = lv_label_create(g);
  lv_label_set_text(lbl, what);
  lv_obj_set_style_text_color(lbl, th->ink_dim, 0);
  lv_obj_set_style_text_font(lbl, th->f_mono, 0);
  lv_obj_center(lbl);
}

lv_obj_t* gauge_render(lv_obj_t* parent, const beacon_theme_t* th, uint8_t pct) {
  if (pct > 100) pct = 100;
  lv_obj_t* g = make_root(parent);
  switch (th->gauge) {
    case GAUGE_BAR:      gauge_bar(g, th, pct);     break;
    case GAUGE_RING:     gauge_ring(g, th, pct);    break;
    case GAUGE_CELL:     gauge_cell(g, th, pct);    break;
    case GAUGE_MEASURE:  gauge_measure(g, th, pct); break;
    case GAUGE_BIGFIG:   gauge_bigfig(g, th, pct);  break;
    case GAUGE_WAVEFORM: gauge_stub(g, th, "~ scope (P-later)"); break;  // bespoke, deferred
    case GAUGE_SUBDIAL:  gauge_stub(g, th, "(o) dial (P-later)"); break;  // bespoke, deferred
  }
  return g;
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

A throwaway demo screen (tap to cycle all 7 themes; eyebrow + theme name + hero figure + a gauge + a finance-style row + hint) + a scripted DataStore smoke run from `setup()` (mock writes + state transitions logged over serial). Flash + verify all 7 themes render correctly, live switch with no reboot (FR-THEME-3), heap holds across switches. Device task — wires the demo + smoke into `main.cpp` for this chunk's verification.

**Files:**
- Create: `firmware/beacon/src/ui/demo_screen.h`
- Create: `firmware/beacon/src/ui/demo_screen.cpp`
- Modify: `firmware/beacon/src/main.cpp` (call `datastore_smoke()` + `demo_screen_init()` from `setup()`)

- [ ] **Step 9.1: Write `demo_screen.h`**

```c
#pragma once
// Throwaway P0-B verification screen: tap to cycle all 7 themes, proving the theme engine +
// live re-render (FR-THEME-3) + fonts + gauges on hardware. Replaced by the real carousel in Chunk C.
void demo_screen_init(void);
```

- [ ] **Step 9.2: Write `demo_screen.cpp`**

The screen registers a rebuild hook via `theme_on_apply(build)`; `theme_set()` calls it with the new tokens. `build()` performs the frozen teardown (clear children + strip the screen's own styles) then rebuilds from the passed `beacon_theme_t*`. The tap handler just advances the index; the clickable flag + event are added once on the screen (they survive `lv_obj_clean`, which only removes children).

```cpp
#include "ui/demo_screen.h"
#include <lvgl.h>
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/gauge.h"
#include "config/layout.h"

// Rebuild hook: frozen teardown order — clear children, strip styles on the screen itself,
// then rebuild from the new tokens (one-theme-resident; heap must hold across switches).
static void build(const beacon_theme_t* th) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, th->bg, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t* eb = lv_label_create(scr);                 // eyebrow (mono)
  lv_label_set_text_fmt(eb, "BEACON / %s", th->id);
  lv_obj_set_style_text_color(eb, th->ink_dim, 0);
  lv_obj_set_style_text_font(eb, th->f_mono, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  lv_obj_t* nm = lv_label_create(scr);                 // theme name (display font)
  lv_label_set_text(nm, th->id);
  lv_obj_set_style_text_color(nm, th->ink, 0);
  lv_obj_set_style_text_font(nm, th->f_display, 0);
  lv_obj_align(nm, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 24);

  lv_obj_t* hero = lv_label_create(scr);               // hero figure (hero font, accent)
  lv_label_set_text(hero, "42%");
  lv_obj_set_style_text_color(hero, th->accent, 0);
  lv_obj_set_style_text_font(hero, th->f_hero, 0);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t* g = gauge_render(scr, th, 42);             // gauge (per theme style)
  lv_obj_align(g, LV_ALIGN_CENTER, 0, 70);

  lv_obj_t* fin = lv_label_create(scr);                // finance-style row (body, up color)
  lv_label_set_text(fin, "S&P 500   +1.2%");
  lv_obj_set_style_text_color(fin, th->up, 0);
  lv_obj_set_style_text_font(fin, th->f_body, 0);
  lv_obj_align(fin, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET - 20);

  lv_obj_t* hint = lv_label_create(scr);               // hint (mono)
  lv_label_set_text(hint, "tap to cycle theme");
  lv_obj_set_style_text_color(hint, th->ink_dim, 0);
  lv_obj_set_style_text_font(hint, th->f_mono, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);
}

static void tap_cb(lv_event_t* e) {
  (void)e;
  theme_set((uint8_t)((theme_index() + 1) % THEME_COUNT));
}

void demo_screen_init(void) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);     // persists across rebuilds (event, not style)
  lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);
  theme_on_apply(build);
  theme_set(0);   // Editorial; triggers build()
}
```

- [ ] **Step 9.3: Add the DataStore smoke + wire the demo in `main.cpp`**

The smoke is a plain function called once from `setup()` (not a FreeRTOS task): it stamps `hdr.last_updated` itself and passes explicit `now` values to `ds_tick_staleness`, so it deterministically exercises LIVE => STALE, then proves ST_ERROR survives a later sweep (state-priority), then HUB_OFFLINE. Surgical edits to the existing `main.cpp`:

Includes + the smoke function above `setup()`:

```cpp
#include <Arduino.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "ui/demo_screen.h"
#include "core/datastore.h"
#include "core/stale.h"

// P0-B on-device DataStore smoke: scripted transitions logged over serial (state-priority proof).
static void datastore_smoke(void) {
  datastore_init();
  weather_rec_t w; memset(&w, 0, sizeof(w));
  w.temp_c = 30.0f; w.hdr.last_updated = 100;
  ds_set_weather(&w);
  LOGI("ds weather=%d (expect LIVE=1)", ds_get_weather().hdr.state);
  ds_tick_staleness(100 + WEATHER_STALE_S);
  LOGI("ds weather=%d after sweep (expect STALE=2)", ds_get_weather().hdr.state);
  ds_set_state_weather(ST_ERROR, ERR_RATE_LIMITED);
  ds_tick_staleness(100 + WEATHER_STALE_S * 10);
  LOGI("ds weather=%d (ERROR not clobbered, expect 4)", ds_get_weather().hdr.state);
  ds_set_hub_offline();
  LOGI("ds usage=%d buddy=%d (expect HUB_OFFLINE=5)",
       ds_get_usage().hdr.state, ds_get_buddy().hdr.state);
  LOGI("ds finance_count=%d", ds_get_finance_count());
}
```

In `setup()`, after `lvgl_port_begin()` succeeds:

```cpp
  datastore_smoke();
  demo_screen_init();
  LOGI("setup done; tap to cycle themes");
```

- [ ] **Step 9.4: Verify (device — flash + observe)** — flash and monitor:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon -t upload && ~/.beacon-pio/bin/pio device monitor
```

Expected on the PANEL: editorial theme paints first (eyebrow `BEACON / editorial`, the theme name in its display font, hero `42%`, the bar gauge, an `S&P 500 +1.2%` row, and a `tap to cycle theme` hint). Each TAP cycles to the next theme — all 7 render with the correct hero/display/body fonts + accent color + gauge style (bar=>ring=>bigfig=>measure=>cell=>waveform-stub=>subdial-stub per the catalog order), no reboot, no corruption (FR-THEME-2 + FR-THEME-3). The two stubs show `~ scope (P-later)` / `(o) dial (P-later)`.

Expected on SERIAL (the `datastore_smoke()` run from `setup()`):
- `ds weather=1 (expect LIVE=1)`
- `ds weather=2 after sweep (expect STALE=2)`
- `ds weather=4 (ERROR not clobbered, expect 4)` — proves the sweep never overwrites ST_ERROR (state-priority rule).
- `ds usage=5 buddy=5 (expect HUB_OFFLINE=5)`
- `ds finance_count=N` (N = `DEFAULT_TICKERS_COUNT`).
- `setup done; tap to cycle themes`, then the loop runs with the watchdog fed (`enableLoopWDT`).

If any theme renders blank text while gauge shapes still draw, `LV_USE_FONT_COMPRESSED` is 0 (Task 6.4) — the fonts are compressed. If a theme renders wrong glyphs (missing chars), revisit the font subset ranges (Task 6). The heap must hold across many taps (each `build()` clears the prior tree before rebuilding); a steady decline points at a missed teardown in the apply hook.

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
| §4.1 beacon_theme_t (+f_hero/radius/stroke_hair/stroke_med) + gauge_style_t + global space/motion | Task 5 (gauge_style.h + theme.h) |
| §4.2 THEMES[7] catalog + theme_set apply-hook model + theme_active/theme_index | Task 5 (data) + Task 7 (engine) |
| §4.3 7 display + body + mono fonts, subsets, manifest, freeze gate | Task 6 + Task 9.5 |
| §4.4 gauge_render bar/ring/cell/measure/bigfig + waveform/subdial deferred | Task 8 |
| §5.1 host unit tests (native env, table-driven) | Tasks 0-5 |
| §5.2 on-device DataStore smoke + theme-switch demo | Task 9 |
| §6 freeze gate (app fits ota_0) | Task 6.6 + Task 9.5 |
| FR-STATE-0 / FR-THEME-1/2/3/4 | Tasks 1-4 (contracts) / Tasks 5-9 (theme) |

### Native/device split (type consistency)

- `BEACON_NATIVE` fences every LVGL/Arduino include. Native compiles only: `records.h`, `screen_state.h`, `hublink.h`, `tickers.h`, `location.h`, `stale.h`, `ds_lock.h` (std::mutex branch), `datastore.{h,cpp}`, `gauge_style.h`, and `theme_catalog.h`. `theme.h` is fully device-side (it includes `<lvgl.h>` unconditionally) and is never included by a native test.
- Device compiles all of the above (FreeRTOS mutex branch) plus `theme.h`, `theme.cpp`, `gauge.cpp`, `demo_screen.cpp`, fonts.
- `build_src_filter = -<*> +<core/datastore.cpp>` (a SINGLE line) makes `datastore.cpp` the ONLY product `.cpp` linked into native; theme/gauge/demo `.cpp` never reach the host.
- `lv_color_t` is RGB565 (`LV_COLOR_DEPTH=16`, confirmed in lv_conf.h) — catalog stores `bt_rgb_t` and converts via `lv_color_make()` device-side, so no LVGL type crosses into native.

### Placeholder scan

- No `TBD`, no "similar to above", no pseudocode. The only literal "deferred"/"placeholder" tokens are the INTENTIONAL `GAUGE_WAVEFORM`/`GAUGE_SUBDIAL` stubs (spec §4.4 explicitly defers them); MANIFEST byte counts are filled with the real measured values during Task 6/9.
- Every code step shows complete, compilable code (real LVGL 8.4 calls — `lv_bar_*`, `lv_arc_*`, `lv_obj_set_style_*`, `lv_label_*`).

### Deviations from spec (additive seams, not contract changes)

- `DEFAULT_TICKERS_COUNT`, `WMO_MAP_COUNT` — derived size helpers the spec implies ("number of entries in DEFAULT_TICKERS").
- Callers stamp `hdr.last_updated` and the sweep takes an explicit `now` (`ds_tick_staleness(now)`) — the LVGL/Arduino-free DataStore carries no clock, so native tests drive deterministic times directly. This is the frozen API (datastore.h), not a deviation.
- `theme_on_apply(theme_apply_cb)` — the screen registers a rebuild hook; `theme_set()` updates the resident tokens then invokes it. The engine holds NO `lv_style_t` and never touches the screen tree (the screen owns its teardown), so there are no `theme_style_*()` accessors. Device-side only.
- `theme_index()` — current-index getter (the spec named it `theme_active_idx`; renamed for brevity, same semantics).
- `WMO_MAP` populated (spec left it as a `/* ... */` comment) with the standard Open-Meteo WMO buckets.
- Non-Editorial theme color hexes are concrete realizations of DESIGN.md's named accents (only Editorial has a full per-token hex table in DESIGN.md); ids/gauge/struct are frozen, hues are tunable on hardware in Task 9.

All frozen header code in Tasks 1, 2, 3 and the `beacon_theme_t`/`gauge_style_t` struct in Task 5 are copied VERBATIM from the spec; the only additions are explicitly-marked derived helpers and device-side engine seams.
