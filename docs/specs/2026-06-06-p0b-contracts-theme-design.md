# P0-B — Frozen Contracts + Theme Engine (design)

> **Status: design (pre-build).** Second of the P0 chunks (after P0-A skeleton/bring-up, complete). Delivers the FROZEN shared contracts every later phase builds against (FR-STATE-0) plus the full theme engine. Once this lands and the contracts are frozen, P1 / P2 / P4 are parallelizable (`prd.md` §7).
>
> Authority: `prd.md` (what), `tech.md` (how — wins on conflict), `DESIGN.md` (token values + theme catalog). Toolchain: `firmware/beacon/README.md`.
>
> Covers: FR-STATE-0 (freeze DataStore / screen_state_t / HubLink / config schemas), FR-THEME-1/2/3/4 (theme engine + all 7 themes + live switch + token-driven gauges), and the data-side of FR-FIN/FR-HOME/FR-USAGE/FR-BUDDY/FR-NOW (their record shapes, not their screens).

## 1. Scope

**In scope:** the frozen contracts (all five domain records, `screen_state_t`, `HubLink`, the two config schemas), a thread-safe `DataStore`, the theme engine (`beacon_theme_t` + `gauge_style_t` + the 7-theme catalog with all fonts), the five token-driven gauge styles, host unit tests (new PlatformIO `native` env), and an on-device theme-switch + DataStore smoke demo.

**Out of scope (later chunks):** the real data fetchers (P1/P2/P4 fill the records — here they are only defined + smoke-filled), NVS persistence of config (chunk D — here config is compiled defaults), the swipe carousel + real screens (chunk C), the BLE `HubLink` implementation (P2), and the two BESPOKE gauges — `GAUGE_WAVEFORM` (oscilloscope trace) + `GAUGE_SUBDIAL` (analog face) — which need custom widgets and land with their themes' screens.

**Decisions taken into this spec:** per-domain typed records (not a tagged union); one comprehensive P0-B (not split); all 7 themes' fonts + all five token gauges built now (only the two bespoke widgets deferred).

## 2. Module layout

```
firmware/beacon/src/
├── core/
│   ├── datastore.{h,cpp}   # thread-safe store; typed get (by-value snapshot) / set; staleness sweep
│   ├── records.h           # FROZEN: record_hdr_t + the 5 domain records + accessors
│   ├── screen_state.h      # FROZEN: screen_state_t + data_err_t + state-priority rule
│   └── hublink.h           # FROZEN: HubLink interface (BLE impl is P2)
├── config/
│   ├── tickers.h           # FROZEN schema: ticker_cfg_t + editable DEFAULT_TICKERS[] + MAX_TICKERS
│   └── location.h          # FROZEN schema: location_cfg_t + DEFAULT_LOCATION + WMO_MAP
└── ui/
    ├── gauge_style.h      # FROZEN: gauge_style_t enum (LVGL-free; shared by catalog + gauge)
    ├── theme.{h,cpp}       # beacon_theme_t + THEMES[7]; theme_set() updates tokens + calls the apply hook
    ├── theme_catalog.h    # LVGL-free catalog data (bt_rgb_t tokens) -> mapped to beacon_theme_t in theme.cpp
    ├── gauge.{h,cpp}       # gauge_render(): bar/ring/cell/measure/bigfig; waveform/subdial -> deferred stub
    └── fonts/              # generated C arrays: 7 hero + 7 display + 5 body + 1 mono (glyph-subset)
test/                       # native-env table-driven unit tests (flat test/test_<unit>/ folders)
```

## 3. Frozen contracts

### 3.1 `screen_state.h` (FROZEN)

```c
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
```

**State priority (frozen rule).** The staleness sweep (§3.3) may only promote `ST_LIVE => ST_STALE`. It must NEVER overwrite `ST_OFFLINE`, `ST_ERROR`, or `ST_HUB_OFFLINE` — those are set explicitly by fetchers/transport and cleared only by a successful update (`ST_LIVE`) or an explicit transition. Precedence high=>low for display: `ST_ERROR` / `ST_OFFLINE` / `ST_HUB_OFFLINE` > `ST_STALE` > `ST_LIVE` > `ST_LOADING`.

### 3.2 `records.h` (FROZEN) — one header + five domain records

**String field rule (frozen):** every `char[]` field is a fixed-capacity, NUL-terminated buffer; writers MUST truncate to fit (never overflow). Capacities are named constants so consumers can size their own buffers:

```c
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
```

```c
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

### 3.3 `datastore.{h,cpp}` (FROZEN API)

Thread-safe per `tech.md` §6: Core-0 data tasks write, Core-1 UI reads snapshots; no I/O ever holds the lock. One FreeRTOS mutex; critical sections are pure struct copies only.

```c
void datastore_init(void);

// Setters (Core-0 fetchers). Copy value fields; force hdr.state=ST_LIVE, hdr.err=ERR_NONE.
// The CALLER stamps hdr.last_updated on the record it passes (the store carries no clock).
void ds_set_weather(const weather_rec_t* r);
void ds_set_finance(uint8_t idx, const finance_rec_t* r);   // idx < MAX_TICKERS
void ds_set_usage(const usage_rec_t* r);
void ds_set_buddy(const buddy_rec_t* r);
void ds_set_nowplaying(const nowplaying_rec_t* r);

// Explicit failure/transport transitions (do not touch the value payload).
void ds_set_state_weather(screen_state_t s, data_err_t e);
void ds_set_state_finance(uint8_t idx, screen_state_t s, data_err_t e);
void ds_set_hub_offline(void);   // flips usage + buddy to ST_HUB_OFFLINE
// ... symmetric setters per domain (full list in records/datastore headers)

// Getters (Core-1 UI). Return a by-value snapshot taken under the lock.
weather_rec_t    ds_get_weather(void);
finance_rec_t    ds_get_finance(uint8_t idx);
uint8_t          ds_get_finance_count(void);
usage_rec_t      ds_get_usage(void);
buddy_rec_t      ds_get_buddy(void);
nowplaying_rec_t ds_get_nowplaying(void);

// Staleness sweep (called ~1/s from a Core-0 timer). For each record: if state==ST_LIVE and
// record_age_s(hdr, now) >= stale_s(source), promote to ST_STALE (boundary is inclusive: an
// age exactly equal to stale_s is stale). Never overwrites ST_OFFLINE/ST_ERROR/ST_HUB_OFFLINE
// (§3.1). stale_s comes from ticker_cfg for finance, per-source constants for the rest.
void ds_tick_staleness(uint32_t now);
```

`finance` is stored as `finance_rec_t finance[MAX_TICKERS]` + `uint8_t finance_count`. **Count contract (frozen):** `datastore_init()` sets `finance_count` from the active ticker config (number of entries in `DEFAULT_TICKERS`, capped at `MAX_TICKERS`) and seeds each slot's `id` from `DEFAULT_TICKERS[idx].id` with `state = ST_LOADING`; slot `idx` always corresponds to ticker `idx`. When NVS-backed config arrives in chunk D, it re-initializes count + ids the same way. Consumers iterate `0..finance_count-1`.

### 3.4 `hublink.h` (FROZEN) — transport-agnostic; BLE impl in P2

```c
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

Frame reassembly (splitting BLE writes on `\n`) lives BELOW this interface in the P2 implementation; `onFrame` always delivers exactly one complete frame.

### 3.5 Config schemas (FROZEN) — compiled defaults now; NVS override is chunk D

`config/tickers.h` — **this is the easily-editable instrument table** (edit + reflash):

```c
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
```

(Exact endpoints/cadences cross-check `docs/research/` §2.3 + `tech.md` §6 cadence table.)

`config/location.h`:

```c
typedef struct {
  float       lat, lon;
  const char* units;       // "metric"
  const char* tz_id;       // IANA, e.g. "Asia/Jakarta"
  const char* ntp_server;
} location_cfg_t;

static const location_cfg_t DEFAULT_LOCATION = { -6.2f, 106.8f, "metric", "Asia/Jakarta", "pool.ntp.org" };

typedef struct { uint16_t code; const char* label; const char* icon; } wmo_entry_t; // icon = glyph/asset id
static const wmo_entry_t WMO_MAP[] = { /* WMO codes -> {short label, icon id}, fixed table */ };
```

## 4. Theme engine

### 4.1 `beacon_theme_t` + `gauge_style_t` (extends `tech.md` §6 — see §7 note)

```c
typedef enum {
  GAUGE_BAR, GAUGE_RING, GAUGE_CELL, GAUGE_WAVEFORM, GAUGE_MEASURE, GAUGE_BIGFIG, GAUGE_SUBDIAL
} gauge_style_t;

typedef struct {
  const char*      id;          // canonical id (DESIGN.md): editorial/hud/calm/blueprint/led/oscilloscope/analog
  lv_color_t       bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  const lv_font_t *f_hero;      // oversized figures (clock, big %): digits + :%°.,+-/ subset
  const lv_font_t *f_display, *f_body, *f_mono; // display=titles/section figures (full ASCII); body/mono per role
  gauge_style_t    gauge;
  uint8_t          glow;        // 0..255 accent glow amount
  uint8_t          radius;      // element corner radius (px)
  uint8_t          stroke_hair; // hairline width (px)
  uint8_t          stroke_med;  // medium stroke width (px)
} beacon_theme_t;
```

Global (not per-theme) tokens, as `#define`s shared across themes (DESIGN.md): `space` rhythm (4/8/12/16/24/32); `motion` durations (`DUR_FAST 120`, `DUR 220`, `DUR_SLOW 400`); easing is a single shared **ease-out** used by all transitions (no per-theme easing). **Reduced-motion** is a global flag (a Settings toggle persisted in chunk D) that motion helpers consult to substitute a crossfade/instant path; the flag's storage is D's, but the motion helpers + the global durations/easing are frozen here so screens animate against a stable contract.

### 4.2 Catalog + switching

A runtime `THEMES[7]` (built once from the LVGL-free `THEME_CATALOG` + the resident fonts in `theme.cpp`) carries the DESIGN.md token values for editorial (default), hud, calm, blueprint, led, oscilloscope, analog. Switching uses an **apply-hook** model: the active screen registers a rebuild callback via `theme_on_apply(theme_apply_cb)`; `theme_set(uint8_t idx)` updates the resident token struct then invokes that hook. The screen's hook performs the **frozen teardown order** (so no live object ever references a freed style):

1. **Clear the active screen's objects** (`lv_obj_clean` on the screen tree).
2. **Strip the screen's own styles** (`lv_obj_remove_style_all` — one-theme-resident rule, `tech.md` §6 — mandatory; the ~199KB internal heap will not tolerate accumulation).
3. **Rebuild from the passed tokens** (no reboot — FR-THEME-3).

`const beacon_theme_t* theme_active(void)` exposes the current theme (NULL before the first `theme_set`); `uint8_t theme_index(void)` exposes the current index. Screens read tokens only (no hardcoded colors/fonts — `tech.md` §6 / coding conventions §10).

### 4.3 Fonts (all 7 themes)

Source the open display fonts per DESIGN.md catalog + a shared body/mono set, glyph-subset, generate LVGL C arrays via `lv_font_conv` (npx), commit them under `ui/fonts/`, and produce a flash-budget note.

| Theme | display | body | mono |
|---|---|---|---|
| editorial | Space Grotesk | Space Grotesk | JetBrains Mono |
| hud | Rajdhani | Rajdhani | JetBrains Mono |
| calm | Doto | Inter | JetBrains Mono |
| blueprint | Chakra Petch | Chakra Petch | JetBrains Mono |
| led | Pixelify Sans | Inter | JetBrains Mono |
| oscilloscope | JetBrains Mono | JetBrains Mono | JetBrains Mono |
| analog | Inter | Inter | JetBrains Mono |

**Per-role glyph subsets:**
- `f_body` + `f_mono` (14-28px): full printable ASCII (incl. lowercase, `&`, `$`) so labels, prompts, track/artist, and error strings render.
- `f_display`: DESIGN.md assigns `font-display` to the now-playing **track title** as well as clock/big-% figures, so it also needs **full printable ASCII + `°`** (not digits-only). To bound flash, the very-large hero sizes (clock / big %, ~60-100px) may use a digits+`:%°.,+-/` variant while a mid display size carries full ASCII for titles — the split is decided by the asset manifest below.

**Asset/flash budget (FREEZE GATE):** generated C font arrays link into the **app slot (~3MB)**, not the littlefs data partition. P0-A used ~647KB of that slot. Produce a measured **font/asset manifest** (per font: family, role, sizes, glyph set, bytes) and confirm the total app image still fits the 3MB `ota_0` slot with margin — this measurement is a gate before the chunk is "done", not an assumption. Currency rendered as text labels (`USD/IDR`, not `$`/`Rp` glyphs) unless the budget allows symbols.

### 4.4 Gauge component

`gauge_render()` switches on `gauge_style_t`. Implement the five token-driven styles now: `GAUGE_BAR`, `GAUGE_RING`, `GAUGE_CELL`, `GAUGE_MEASURE`, `GAUGE_BIGFIG`. The two BESPOKE styles `GAUGE_WAVEFORM` (oscilloscope trace) and `GAUGE_SUBDIAL` (analog face) render a labeled placeholder and are completed with their themes' screens (they need custom widgets, DESIGN.md §"Outlier widgets").

## 5. Verification

### 5.1 Host unit tests (new PlatformIO `native` env) — table-driven

- **Staleness + state priority:** table of (initial state, age, stale_s, expected state) — asserts LIVE=>STALE at the boundary and that OFFLINE/ERROR/HUB_OFFLINE are never clobbered.
- **`record_age_s`:** table incl. the never-updated (`last_updated==0` => `UINT32_MAX`) case.
- **Config validation:** `DEFAULT_TICKERS` ids unique, count <= `MAX_TICKERS`, every enum in range.
- **Theme lookup:** `THEMES[]` has 7 entries, ids match the DESIGN.md canonical set, no NULL font pointers.
- **DataStore set/get:** set then get returns an equal snapshot; finance idx isolation (writing slot i doesn't touch slot j); `datastore_init` seeds `finance_count` + slot ids from `DEFAULT_TICKERS`.
- **Explicit state + recovery (table-driven):** `ds_set_hub_offline()` flips both usage + buddy to `ST_HUB_OFFLINE`; a subsequent successful `ds_set_usage/ds_set_buddy` clears it back to `ST_LIVE`; explicit failure setters (`ds_set_state_*`) change state/err WITHOUT mutating the value payload; a `ds_set_*` success clears a prior `ST_ERROR`.

(The `native` env stubs LVGL/Arduino types it doesn't need; pure-logic units only. Hardware-coupled pieces are covered in §5.2.)

### 5.2 On-device smoke + theme demo

- **DataStore smoke:** a Core-0 task writes mock records + drives state transitions (LIVE=>STALE=>ERROR=>recover); serial logs confirm the sweep + priority rules on real hardware, and the UI reads snapshots without blocking.
- **Theme-switch demo:** a throwaway screen (tap to cycle all 7 themes) showing a representative layout — eyebrow + big figure + a gauge + a finance-style row — that re-renders per theme with the correct fonts/colors/gauge, proving FR-THEME-2 (all 7) and FR-THEME-3 (live switch, no reboot) on hardware. Watch the internal-heap floor across switches (proves `theme_set` releases styles).

## 6. Freeze checklist (end of P0-B)

Frozen by THIS chunk, P1/P2/P4 may depend on them: `screen_state_t` + `data_err_t` + state-priority rule; `record_hdr_t` + the five domain records (incl. string-length constants + the now-playing art-ref contract) + `record_age_s`; the `DataStore` get/set/sweep API + `finance_count` init contract; the `HubLink` interface + its send/callback semantics; `ticker_cfg_t` + `MAX_TICKERS` + `location_cfg_t` + `wmo_entry_t` (schemas; values stay editable); `beacon_theme_t` + `gauge_style_t` + the global space/motion tokens.

Already frozen by P0-A (later phases also depend on these; not re-frozen here): `SAFE_INSET=40` + `CORNER_R=90` + `LCD_X/Y_OFFSET=8` (`config/layout.h`) and `partitions.csv`.

**Freeze gate (must pass before "done"):** the measured font/asset manifest (§4.3) confirms the app image fits the 3MB `ota_0` slot with margin.

## 7. Doc-sync note

`beacon_theme_t` here ADDS `radius`, `stroke_hair`, `stroke_med` beyond the struct printed in `tech.md` §6 (which omits the DESIGN.md `radius`/`stroke` tokens). `tech.md` §6 and DESIGN.md's "Token authority" line say to keep the struct and the token list in sync — so this spec's struct is authoritative and `tech.md` §6 should be updated to match when P0-B lands.
