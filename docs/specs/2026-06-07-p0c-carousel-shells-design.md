# P0-C: Swipe Carousel + 6 Screen Shells — Design

**Goal:** Build the horizontal swipe carousel and the six on-device screens (Home, Finance, AI Usage, Coding Buddy, Now-Playing, Settings) as fully state-aware shells wired to the frozen P0-B DataStore + theme engine, so P1/P2/P4 only add data fetchers behind complete screens.

**Architecture:** All six screen roots live in one LVGL scroll-snap container (finger-following slide + snap); the LVGL object pool is relocated to PSRAM so six resident screens cost abundant external RAM, not scarce internal SRAM, while DMA draw buffers stay internal. Screens read by-value DataStore snapshots and render through a theme-owned shared-style set; a per-state visual system covers loading/live/stale/offline/error/hub-offline.

**Tech stack:** ESP32-S3 (8 MB OPI PSRAM, 16 MB flash), Arduino-ESP32 3.3.5 (pioarduino 55.03.35), LVGL 8.4.0, the P0-B contracts (`DataStore`, `screen_state_t`, theme engine, gauges, fonts).

**Consumes (frozen in P0-B, not modified here):** `core/datastore.{h,cpp}`, `core/records.h`, `core/screen_state.h`, `ui/theme.{h,cpp}`, `ui/theme_catalog.h`, `ui/gauge.{h,cpp}`, `ui/fonts/`, `config/tickers.h`, `config/location.h`.

---

## 1. Scope

| In P0-C | Deferred (phase) |
|---|---|
| Finger-following scroll-snap carousel, 6 resident screens, dot position indicator (FR-PLAT-2) | NVS persistence of last-screen/brightness/theme (P0-D, FR-PLAT-3) |
| All six screen layouts, **fully state-aware** (loading/live/stale+age/offline/error/hub-offline) | Real data fetchers: weather/finance (P1), usage/buddy via BLE (P2), now-playing via Spotify (P4) |
| Read from the frozen DataStore + theme engine | Time service NTP+RTC (P0-D, FR-PLAT-8); sleep/idle dim (P0-D, FR-PLAT-7) |
| Settings: Theme switch + Brightness wired **live** (no persistence) | Settings WiFi/Sleep/Tickers actions + persistence (P0-D) |
| Safe-area compliance on every screen (FR-PLAT-4) | WiFi provisioning captive portal (P0-D) |
| Dev-only state-driver to exercise all states on-device | — |

**Phasing shift (recorded):** choosing the full state model now moves **FR-STATE-1/2 into P0-C**. P1 is reduced to wiring real fetchers behind already-complete, already-state-aware screens. The PRD/tech phasing tables are updated to reflect this at spec sign-off.

**Requirements covered:** FR-PLAT-2 (carousel + indicator), FR-PLAT-4 (safe area), FR-THEME-2/3 (all 7 themes render + live switch, already satisfied in P0-B and re-exercised here on real screens), FR-STATE-1/2 (per-screen state model), FR-STATE-3 (UI never crashes/hangs on failure), partial coverage of FR-HOME/FR-FIN/FR-USAGE/FR-BUDDY/FR-NOW (layout + state shells; data wiring in their phases), FR-SET-3 (theme) + FR-SET-2 (brightness) live but unpersisted.

---

## 2. Architecture decision: carousel memory model

**Decision:** all six screens resident in an LVGL scroll-snap container (Approach A) **plus** relocating the LVGL object pool to PSRAM. Rationale, after a second-opinion review (codex) + LVGL/Espressif research:

- A label-heavy shell is ~5-12 KB of LVGL heap; six of them (~30-70 KB) do **not** fit the current fixed 48 KB internal `LV_MEM` pool with margin.
- An LVGL scroll-snap container does **not** virtualize children: all six object trees stay allocated (a **heap** cost). But LVGL only redraws the invalidated, clipped, visible area (a **render** cost proportional to one screen, not six), rasterizing into the internal draw buffer and flushing via internal DMA.
- Therefore six resident screens is a heap bill, payable from the 8 MB OPI PSRAM, while render stays fast and the flush path never touches PSRAM. This is the Espressif-sanctioned split (object canvas in PSRAM, DMA transfer buffer in internal SRAM).
- Net win: the 48 KB internal `.bss` pool is returned to the scarce internal SRAM, protecting the >=60 KB free-internal-heap floor under WiFi+BLE+TLS in P2.

**Implementation:** `lv_conf.h` sets `LV_MEM_CUSTOM 1`; a small allocator (`ui/lv_mem_psram.cpp`) backs LVGL alloc/free/realloc with `heap_caps_*(…, MALLOC_CAP_SPIRAM)`. DMA draw buffers are unchanged (internal, with the existing PSRAM fallback). `lv_mem_monitor` is unavailable under custom alloc, so heap is observed via `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` plus the internal-floor log already in `lvgl_port`.

Note `LV_MEM_CUSTOM` routes ALL LVGL dynamic allocations to PSRAM, not just the object tree: transient scratch (large opacity/transform layers, style-property growth) lands there too. P0-C therefore avoids full-screen opacity/transform effects and watches the PSRAM largest-free block during swipe and theme switch (not just total free).

**Fallback (documented, not built):** if on-device profiling shows object-tree traversal or PSRAM contention is the bottleneck, switch the carousel to a 3-page sliding window (prev/current/next built, recycle on snap). The `screen_module_t` interface is identical either way, so the change is localized to `carousel.cpp`.

**The mandatory on-device gate:** log, during a worst-case swipe + a theme switch, BOTH (a) min free internal heap + largest free internal DMA block (draw buffers + radios) and (b) min free PSRAM + largest free PSRAM block + any LVGL allocation failure (object pool). Re-check under active BLE + TLS in P2.

---

## 3. Components

### 3.1 `ui/lv_mem_psram.{h,cpp}`
PSRAM-backed allocator for `LV_MEM_CUSTOM`. Exposes `lvm_alloc/lvm_free/lvm_realloc` wired into `lv_conf.h` via `LV_MEM_CUSTOM_ALLOC` etc. Each call delegates to `heap_caps_malloc/free/realloc` with `MALLOC_CAP_SPIRAM`. The header must be C-compatible (`lv_conf.h` includes it via `LV_MEM_CUSTOM_INCLUDE`).

**Boot-order requirement (mandatory):** the PSRAM heap must be initialized before the first LVGL allocation. `setup()` does a RUNTIME PSRAM check (`heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0` / `psramFound()`) and **fails fast** (logs + halts) BEFORE `lv_init()` if PSRAM is absent — the compile-time `BOARD_HAS_PSRAM` flag is necessary but not sufficient. Sequence: power -> display -> `psram check` -> `lvgl_port_begin` (which calls `lv_init`) -> carousel.

### 3.2 `ui/styles.{h,cpp}` — theme-owned shared styles
A single set of `lv_style_t` rebuilt from the active `beacon_theme_t`: `st_screen` (bg), `st_eyebrow` (mono + accent), `st_label` (mono + ink_dim), `st_display`, `st_hero`, `st_body`, `st_value_up`, `st_value_down`, `st_hairline`, `st_chip` (state badge), `st_dim` (stale/last-value). Screens attach styles via `lv_obj_add_style` only; **no per-object color/font setters** (so a theme switch restyles everything centrally).

`styles_apply(const beacon_theme_t*)` rebuilds the set and calls `lv_obj_report_style_change(NULL)` (valid in LVGL 8.4 to re-evaluate all objects using these styles). It is registered as the theme apply hook (`theme_on_apply(styles_apply)`): switching themes mutates the shared styles in place and reports the change; no screen object is destroyed or rebuilt (avoids PSRAM-pool churn/fragmentation across switches with six resident screens).

This is consistent with the frozen P0-B theme contract, NOT a change to it: `theme.h`'s apply hook is screen-owned and only requires that the hook make the screen reflect the new tokens. P0-B's throwaway demo satisfied it by clear+rebuild; P0-C satisfies it by shared-style restyle. The contract (`theme_set` updates tokens then invokes the hook) is unchanged. The spec sign-off records that the real-screen apply strategy is shared-style restyle.

### 3.3 `ui/screen.h` — screen module interface
```c
typedef struct {
  const char* id;                     // eyebrow id: "HOME","MARKETS","LIMITS","CLAUDE","NOW","SETTINGS"
  lv_obj_t*  (*build)(lv_obj_t* page); // build the screen into its page container; returns the page
  void       (*update)(void);          // re-render from the current DataStore snapshot (+ recompute age)
} screen_module_t;
```
`build()` runs once at boot. `update()` is idempotent and reads only `ds_get_*()` by-value snapshots; it is safe to call repeatedly. No `destroy()` (Approach A keeps all resident); a `destroy` hook is added only if the fallback window model is adopted.

### 3.4 `ui/carousel.{h,cpp}`
- Builds a horizontal flex container (`LV_FLEX_FLOW_ROW`) with scroll-snap-x center, `LV_OBJ_FLAG_SCROLL_ONE`, snappable children; one page per module, each sized `SCREEN_W x SCREEN_H`. The pager is constrained to horizontal scroll only: `lv_obj_set_scroll_dir(pager, LV_DIR_HOR)`.
- Calls each module's `build(page)` at init (screen order below), and `styles_apply` for the initial theme.
- A scroll/scroll-end event resolves the current index, updates the dot indicator, and calls `update()` on the visible page plus its immediate neighbors.
- A repeating LVGL timer (~500 ms, Core-1) calls ONLY the visible screen's `update()` (the read path: re-renders + recomputes age). It does **not** write DataStore state. The staleness sweep (`ds_tick_staleness`, a write-side transition the DataStore contract assigns to Core-0) runs on a separate **Core-0** ticker (an `esp_timer` or a small FreeRTOS task at ~1 s); in C this Core-0 ticker is owned by `dev_seed` (P1's real fetchers take it over). The DataStore lock keeps the cross-core access safe; the split keeps read (Core-1) and write (Core-0) paths clean per `datastore.h`.
- Dot indicator: a row of one dot per screen (6), the active dot `accent` and others `line`. It is placed on `lv_layer_top()` (a non-clickable overlay sibling of the scroll container, NOT a child of it) so it does not scroll with the pages, positioned inside the safe area in the bottom arc-free band. Pages reserve that bottom band so content never underlaps it.
- Screen order (PRD section 1): **Home, Finance(MARKETS), Usage(LIMITS), Buddy(CLAUDE), Now-Playing(NOW), Settings**. All six always present (enable/disable is a P0-D NVS concern).
- Pure index helpers (`carousel_next/prev/clamp` given count + current) are extracted as free functions for host testing; the wrap policy is **clamp at ends (no wraparound)**.

### 3.5 `ui/state_view.{h,cpp}` — per-state rendering
A shared treatment applied per data region so all six screens render states consistently:

| `screen_state_t` | Treatment |
|---|---|
| `ST_LOADING` | dim em-dash placeholders (`--`) in value slots |
| `ST_LIVE` | normal (matches the mockups) |
| `ST_STALE` | value shown via `st_dim` + age badge (`2m`/`1h`, mono) in the top-right status slot |
| `ST_OFFLINE` | `OFFLINE` chip + last value dimmed + age |
| `ST_ERROR` | `ERR` chip + reason glyph (per `data_err_t`) |
| `ST_HUB_OFFLINE` | `HUB OFFLINE` chip (usage/buddy only) + last value + age |
| `ST_RECONNECTING` | actions disabled (buddy); chip `RECONNECTING` |

The top-right slot each mockup uses for time / `POLL 30S` / `REQ A1B2` doubles as the **status slot**: LIVE shows the screen's normal right-header; any non-live state replaces it with the chip. Pure helpers — `age_str(uint32_t age_s)` and `state_label(screen_state_t, data_err_t)` — are host-tested.

### 3.6 `ui/screens/screen_*.{h,cpp}`
Six modules, each a `screen_module_t` built to its editorial mockup (the editorial lane in `docs/design/mockups/directions.html`), every value routed through `state_view`:

- **home** (`HOME`): clock (hero font) + date line (mono) + hairline + weather (temp / humidity / condition) from `weather_rec_t`. The clock, date line, and the top-right `Wnn . dd.mm` (week + date) header ALL depend on P0-D's time service, so they render placeholders (`--:--`, `--`) in C; the dev-seeder can fake a monotonic time for visual verification. Weather is the only live-data region on this screen in C.
- **finance** (`MARKETS`): a row per active ticker (`ds_get_finance_count()` slots), each `id` (dim) | value (display) | signed change with `^`/`v` glyph + `up`/`down` color (FR-FIN-2: sign + glyph + color, never color alone). With >6 tickers the list scrolls vertically inside the page; horizontal swipe still pages (LVGL gesture-direction routing). Per-row state via `state_view` (a slot may be stale while others live).
- **usage** (`LIMITS`): title `AI USAGE`; CLAUDE then CODEX, each a 2x2 (5H / 7D) of big-% + thin bar + `resets …`, from `usage_rec_t`. An unavailable window (`pct < 0`, i.e. JSON null) renders `--` and **no bar** — the negative sentinel must never be cast into a `uint8_t` bar/gauge argument; only `0..100` reaches the bar. `reset == 0` renders the reset as unknown. Whole-screen `HUB OFFLINE` state.
- **buddy** (`CLAUDE`): status line (`running . waiting . tokens . ctx%`) + two layouts chosen by the discriminator `buddy_rec_t.prompt.present`: **idle** (`present == false` => recent activity entries) and **prompt** (`present == true` => `PERMISSION -- APPROVE?`, `prompt.tool` in display font, `prompt.hint` in a bordered box, `DENY | APPROVE` action bar). Tap Approve/Deny in C is a **local stub** (logs + clears the prompt); the real hub round-trip + fail-closed timeout is P2. Actions are disabled under `ST_HUB_OFFLINE` and `ST_RECONNECTING`. Whole-screen `HUB OFFLINE`.
- **nowplaying** (`NOW`): track title / artist / target device + progress bar + play state from `nowplaying_rec_t`; a **no-active-device** state (`has_device == false`). Transport controls are stubbed (Spotify is P4). Album art is a reference only (P0-B contract) — not rendered in C.
- **settings** (`SETTINGS`): rows Wi-Fi / Brightness / Theme / Tickers / Sleep / About. **Theme** (tap cycles or opens a picker -> `theme_set`, live) and **Brightness** (adjust -> existing DCS 0x51 backlight path, live) are wired; Wi-Fi (status text), Tickers (`N assets` from `ds_get_finance_count()`), Sleep, About are display-only until P0-D. No values persist in C. Settings has no DataStore record, so it is NOT part of the `state_view` data-state model — its only "states" are local row enabled/display-only. (The `fully state-aware` claim in section 1 applies to the five data screens.)

### 3.7 `ui/dev_seed.{h,cpp}` — dev-only verification harness
Gated behind a `BEACON_DEV` build flag (compiled out of release). Seeds representative values into the DataStore so screens render populated like the mockups, and binds a long-press that cycles the current screen's record through the states **reachable for its plane via the frozen DataStore API** — this is constrained by what setters exist:
- **Device-plane (weather/finance/nowplaying):** full cycle LIVE -> STALE (via Core-0 sweep) -> OFFLINE -> ERROR -> back, using `ds_set_state_weather/finance/nowplaying(s, e)`.
- **Hub-plane (usage/buddy):** LIVE -> STALE (sweep) -> HUB_OFFLINE (`ds_set_hub_offline`) -> LIVE. These records have no `ds_set_state_*` setter by design (the BLE hub is their only transport), so ERROR/RECONNECTING are P2 concerns driven by the real hub link, not the C dev-driver. `state_view` still renders them; they are exercised in P2.

The dev-driver also provides the Core-0 `ds_tick_staleness` ticker and a fake monotonic time for the home clock (both `BEACON_DEV`-only; real epoch time + fetchers replace them in P0-D/P1). It is the harness for the deploy-each-checkpoint loop and the FR-STATE-3 fault-injection check.

### 3.8 `main.cpp`
Replaces the P0-B demo wiring: after `lvgl_port_begin()`, call `carousel_init()` then `dev_seed_init()` (under `BEACON_DEV`). The orphaned P0-A `ui/test_screen.{h,cpp}` and the P0-B `ui/demo_screen.{h,cpp}` are removed (superseded by the carousel).

---

## 4. Data flow & lifecycle

`dev_seed` (dev, Core-0) -> DataStore. **Write path (Core-0):** the staleness sweep `ds_tick_staleness(now)` runs ~1 s on a Core-0 ticker (owned by `dev_seed` in C; `now` is a `BEACON_DEV` monotonic base from `millis()/1000`, replaced by the RTC/NTP epoch in P0-D — both the seed timestamps and `now` share the same base so ages are correct). **Read path (Core-1):** an LVGL timer (~500 ms) calls the visible screen's `update()`, which reads `ds_get_*()` by-value snapshots, recomputes age, and renders via shared styles + `state_view`. Theme switch -> `styles_apply` rebuilds styles + `lv_obj_report_style_change(NULL)` -> all resident screens restyle live. Swipe -> scroll-snap -> index change -> dot indicator + neighbor `update()`. Only the visible screen updates per tick, so cost is one screen, not six. The DataStore lock guards the Core-0 write / Core-1 read overlap.

## 5. Error handling (FR-STATE-3)

By-value snapshot getters (no dangling references across the lock), `state_view` handles every enum including a default branch, and nothing on Core 1 blocks. The dev state-driver injects ERROR/OFFLINE/HUB_OFFLINE to prove the UI stays responsive (swipe still works, no freeze) under bad/missing data.

## 6. Testing

**Host (native, table-driven Unity):**
- carousel index math (`next/prev/clamp`, ends clamp, single-screen edge case).
- `age_str` (0 s, seconds, minutes, hours, never-updated).
- `state_label(state, err)` mapping incl. each `data_err_t`.
- finance formatting: value grouping + signed change/glyph + up/down selection.

**On-device (deploy-each-checkpoint):**
- swipe both directions through all six; dot indicator tracks; finger-follow feel; subjective >=30 FPS.
- each screen renders correctly in editorial + spot-check two other themes; a live theme switch restyles all six without a reboot.
- state-driver cycles a screen through its reachable states; each treatment renders per the table.
- **heap gate:** during a worst-case swipe AND a theme-switch stress (cycle all 7 themes), log min free internal heap + largest free internal DMA block (must hold the >=60 KB floor with margin) AND min free PSRAM + largest free PSRAM block + assert no LVGL allocation failure (the object pool). A steadily falling PSRAM largest-block across theme switches signals pool fragmentation -> revisit shared-style strategy.

## 7. Functional checkpoints (each deployed + verified before the next)

1. PSRAM `LV_MEM` pool + shared-styles theme integration — the P0-B visual still renders; internal heap is measurably freed.
2. Carousel container + six empty pages + dot indicator + finger-follow nav.
3. Screen modules added incrementally: home -> finance -> usage -> buddy -> nowplaying -> settings (deploy after each).
4. `state_view` + dev state-driver — all states verified on-device.
5. Settings Theme + Brightness live.
6. Integration: heap gate + multi-theme pass + safe-area re-check.

## 8. File structure

```
firmware/beacon/src/ui/
  lv_mem_psram.{h,cpp}
  styles.{h,cpp}
  screen.h
  carousel.{h,cpp}
  state_view.{h,cpp}
  dev_seed.{h,cpp}
  screens/
    screen_home.{h,cpp}
    screen_finance.{h,cpp}
    screen_usage.{h,cpp}
    screen_buddy.{h,cpp}
    screen_nowplaying.{h,cpp}
    screen_settings.{h,cpp}
firmware/beacon/test/
  test_carousel/test_main.cpp
  test_state_view/test_main.cpp
  test_fmt/test_main.cpp
```
(Matches tech.md section 12's `ui/` + `screens/` layout.)

## 9. Risks & open items

- **Nested scroll (finance):** vertical list scroll inside the horizontal pager relies on LVGL gesture-direction routing; verify on-device that a vertical drag scrolls the list and a horizontal drag pages, without a "sticky" axis. Mitigation if it fights: constrain finance to <=6 visible rows in C (the default set fits) and defer long-list scroll polish to P1.
- **PSRAM during flash writes:** PSRAM is disabled while flash is being written (NVS in P0-D, OTA later). Because the LVGL object pool now lives in PSRAM, this is broader than flush tearing: any LVGL object/style access during the cache-off window can fault. P0-D must quiesce or lock LVGL (suspend the render task / take a mutex) around NVS writes. (No flash writes happen in C itself, so this is a constraint handed to P0-D, recorded here.)
- **LVGL scratch in PSRAM:** `LV_MEM_CUSTOM` routes transient layer/scratch allocations to PSRAM too; large opacity/transform layers are avoided in C, and the heap gate watches the PSRAM largest-free block during swipe + theme switch.
- **Heap gate:** the all-resident model is validated only on-device; the gate in checkpoint 6 is authoritative. Fallback to the 3-page window is pre-scoped.
- **Brightness path:** assumes the P0-A display brightness (DCS 0x51) is callable from a thin `display_set_brightness(uint8_t)`; add the wrapper if not already exposed.

## 10. Out of scope (explicit)

NVS/persistence, WiFi provisioning, time service, sleep/idle, real data fetchers, the bespoke Analog face + Oscilloscope trace widgets (still deferred per P0-B), album-art decoding, and IMU/long-press product gestures (the dev long-press is dev-only).
