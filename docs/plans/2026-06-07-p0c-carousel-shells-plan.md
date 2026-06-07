# P0-C Carousel + 6 Screen Shells Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the horizontal swipe carousel and six fully state-aware screen shells (Home, Finance, Usage, Buddy, Now-Playing, Settings) wired to the frozen P0-B DataStore + theme engine, with the LVGL object pool relocated to PSRAM.

**Architecture:** All six screen roots live in one LVGL scroll-snap container (finger-following slide). The LVGL object pool moves to PSRAM (`LV_MEM_CUSTOM=1`) so six resident screens cost external RAM, not the scarce internal SRAM; DMA draw buffers stay internal. Screens read by-value DataStore snapshots and render through a theme-owned shared-style set restyled live on theme switch; per-state visuals (loading/live/stale/offline/error/hub-offline) come from pure, host-tested formatting helpers.

**Tech Stack:** ESP32-S3 (8 MB OPI PSRAM), Arduino-ESP32 3.3.5 (pioarduino 55.03.35), LVGL 8.4.0, P0-B contracts (`DataStore`, `screen_state_t`, theme engine, gauges, fonts).

**Spec:** `docs/specs/2026-06-07-p0c-carousel-shells-design.md`.

**Conventions (apply to every task):**
- All PlatformIO commands use the venv binary: `~/.beacon-pio/bin/pio`.
- Device build/flash: `cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon -t upload && ~/.beacon-pio/bin/pio device monitor` (port auto-detected; current `/dev/cu.usbmodem*`).
- Host tests: `cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native`.
- ASCII only in code/comments (`=>` not arrows). Match P0-A/B style: focused files, comments explain *why*.
- **Commits are the USER's to make.** Each task ends with a suggested `git add`/message; stage but do not commit.
- Pure helpers are header-only `static inline` (LVGL-free) so native tests `#include` them with no `build_src_filter` change. LVGL-coupled code is device-only (not host-tested), verified on-device like P0-B's theme.cpp/gauge.cpp.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/ui/lv_mem_psram.{h,cpp}` | PSRAM allocator for `LV_MEM_CUSTOM` (heap_caps SPIRAM) |
| `src/lv_conf.h` (modify) | `LV_MEM_CUSTOM 1` + custom-alloc include |
| `src/ui/styles.{h,cpp}` | theme-owned shared `lv_style_t` set; rebuilt live on theme switch |
| `src/ui/screen.h` | `screen_module_t` interface `{id, build(page), update()}` |
| `src/ui/carousel_nav.h` | pure index math (host-tested) |
| `src/ui/carousel.{h,cpp}` | scroll-snap pager + dot indicator + per-tick update |
| `src/ui/state_view.h` | pure state formatting/predicates (host-tested) |
| `src/ui/fmt.h` | pure finance value/change formatting (host-tested) |
| `src/ui/screens/screen_home.{h,cpp}` | Home shell |
| `src/ui/screens/screen_finance.{h,cpp}` | Finance/MARKETS shell |
| `src/ui/screens/screen_usage.{h,cpp}` | Usage/LIMITS shell |
| `src/ui/screens/screen_buddy.{h,cpp}` | Buddy/CLAUDE shell |
| `src/ui/screens/screen_nowplaying.{h,cpp}` | Now-Playing/NOW shell |
| `src/ui/screens/screen_settings.{h,cpp}` | Settings shell (theme+brightness live) |
| `src/ui/dev_seed.{h,cpp}` | dev-only seeder + state-driver + Core-0 staleness ticker |
| `src/main.cpp` (modify) | PSRAM check, carousel_init, dev_seed_init |
| `src/platformio.ini` (modify) | `-DBEACON_DEV=1` in `[env:beacon]` |
| remove `src/ui/test_screen.{h,cpp}`, `src/ui/demo_screen.{h,cpp}` | superseded |
| `test/test_carousel/`, `test/test_state_view/`, `test/test_fmt/` | host tests |

---

## Task 0: LVGL object pool in PSRAM + BEACON_DEV flag

Move the LVGL heap to PSRAM, fail fast if PSRAM is absent, and confirm the existing P0-B-equivalent UI still renders with the 48 KB internal `.bss` pool freed.

**Files:**
- Create: `src/ui/lv_mem_psram.h`, `src/ui/lv_mem_psram.cpp`
- Modify: `src/lv_conf.h`, `src/platformio.ini`, `src/main.cpp`

- [ ] **Step 0.1: Write `lv_mem_psram.h`**

```c
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// LVGL custom allocator backed by external PSRAM (LV_MEM_CUSTOM=1). Keeps the LVGL
// object pool out of scarce internal SRAM; DMA draw buffers stay internal (lvgl_port).
void* lvm_alloc(size_t size);
void  lvm_free(void* p);
void* lvm_realloc(void* p, size_t size);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 0.2: Write `lv_mem_psram.cpp`**

```cpp
#include "ui/lv_mem_psram.h"
#include <esp_heap_caps.h>

void* lvm_alloc(size_t size)            { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
void  lvm_free(void* p)                 { heap_caps_free(p); }
void* lvm_realloc(void* p, size_t size) { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM); }
```

- [ ] **Step 0.3: Point `lv_conf.h` at the PSRAM allocator**

In `src/lv_conf.h`, set `LV_MEM_CUSTOM` to 1 and fill the custom block. Replace the existing `#define LV_MEM_CUSTOM 0` and its custom branch values:

```c
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM == 0
    #define LV_MEM_SIZE (48U * 1024U)
    #define LV_MEM_ADR 0
    #if LV_MEM_ADR == 0
        #undef LV_MEM_POOL_INCLUDE
        #undef LV_MEM_POOL_ALLOC
    #endif
#else
    #define LV_MEM_CUSTOM_INCLUDE "ui/lv_mem_psram.h"
    #define LV_MEM_CUSTOM_ALLOC   lvm_alloc
    #define LV_MEM_CUSTOM_FREE    lvm_free
    #define LV_MEM_CUSTOM_REALLOC lvm_realloc
#endif
```

(Only the `LV_MEM_CUSTOM` value flips to 1 and the `#else` branch is filled; the `==0` branch is left intact but unused.)

- [ ] **Step 0.4: Add the runtime PSRAM fail-fast in `main.cpp` setup()**

Add `#include <esp_heap_caps.h>` and, in `setup()` immediately BEFORE `lvgl_port_begin()` (which calls `lv_init()` -> first PSRAM alloc), insert:

```cpp
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { LOGE("halt: no PSRAM (LVGL pool needs it)"); return; }
  LOGI("psram total=%u free=%u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
```

- [ ] **Step 0.5: Add `-DBEACON_DEV=1` to `[env:beacon]` build_flags** (after `-DBOARD_HAS_PSRAM`):

```ini
  -DBEACON_DEV=1            ; compiles in the dev seeder + state-driver (Chunk-C verification harness)
```

- [ ] **Step 0.6: Verify (device build only — full UI comes later)** — at this point `main.cpp` still calls the P0-B `demo_screen` (removed in Task 12). Build to confirm the PSRAM pool compiles + links and the demo still renders:

```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon -t upload && ~/.beacon-pio/bin/pio device monitor
```
Expected serial: `psram total=8388608 free=…`, then the P0-B demo renders. Confirm the LVGL UI still draws (text + gauge) — proving objects now allocate from PSRAM. The internal-heap log from `lvgl_port` should show ~48 KB more free internal than before.

- [ ] **Step 0.7: Commit checkpoint** — stage; ask USER to commit:
```bash
git add firmware/beacon/src/ui/lv_mem_psram.h firmware/beacon/src/ui/lv_mem_psram.cpp firmware/beacon/src/lv_conf.h firmware/beacon/src/platformio.ini firmware/beacon/src/main.cpp
```
Message: `feat(p0c): relocate LVGL object pool to PSRAM + dev flag`

---

## Task 1: Shared theme styles (`styles.{h,cpp}`)

A theme-owned `lv_style_t` set; theme switch rebuilds the set + `lv_obj_report_style_change(NULL)` (no screen rebuild).

**Files:**
- Create: `src/ui/styles.h`, `src/ui/styles.cpp`

- [ ] **Step 1.1: Write `styles.h`**

```c
#pragma once
#include <lvgl.h>

// Theme-owned shared styles. Screens attach these via lv_obj_add_style ONLY (no per-object
// color/font setters), so a theme switch restyles every screen by mutating these + reporting.
typedef struct {
  lv_style_t screen;     // bg
  lv_style_t eyebrow;    // mono, accent ("BEACON / X")
  lv_style_t slot;       // mono, ink_dim (top-right status slot / hints / dim labels)
  lv_style_t display;    // display font, ink (titles / values)
  lv_style_t hero;       // hero font, ink (clock / big figures)
  lv_style_t body;       // body font, ink
  lv_style_t up;         // text_color = up
  lv_style_t down;       // text_color = down
  lv_style_t accent;     // text_color = accent
  lv_style_t hairline;   // bg = line (1px rule objects)
  lv_style_t dim;        // text_color = ink_dim (overlay to dim a stale value)
} app_styles_t;

extern app_styles_t S;

void styles_init(void);   // lv_style_init all + register theme apply hook (call once, before building screens)
```

- [ ] **Step 1.2: Write `styles.cpp`**

```cpp
#include "ui/styles.h"
#include "ui/theme.h"

app_styles_t S;

static void styles_apply(const beacon_theme_t* t) {
  lv_style_set_bg_color(&S.screen, t->bg);
  lv_style_set_bg_opa(&S.screen, LV_OPA_COVER);

  lv_style_set_text_font(&S.eyebrow, t->f_mono);
  lv_style_set_text_color(&S.eyebrow, t->accent);

  lv_style_set_text_font(&S.slot, t->f_mono);
  lv_style_set_text_color(&S.slot, t->ink_dim);

  lv_style_set_text_font(&S.display, t->f_display);
  lv_style_set_text_color(&S.display, t->ink);

  lv_style_set_text_font(&S.hero, t->f_hero);
  lv_style_set_text_color(&S.hero, t->ink);

  lv_style_set_text_font(&S.body, t->f_body);
  lv_style_set_text_color(&S.body, t->ink);

  lv_style_set_text_color(&S.up, t->up);
  lv_style_set_text_color(&S.down, t->down);
  lv_style_set_text_color(&S.accent, t->accent);

  lv_style_set_bg_color(&S.hairline, t->line);
  lv_style_set_bg_opa(&S.hairline, LV_OPA_COVER);

  lv_style_set_text_color(&S.dim, t->ink_dim);

  lv_obj_report_style_change(NULL);   // re-evaluate every object using these styles
}

void styles_init(void) {
  lv_style_init(&S.screen);  lv_style_init(&S.eyebrow); lv_style_init(&S.slot);
  lv_style_init(&S.display); lv_style_init(&S.hero);    lv_style_init(&S.body);
  lv_style_init(&S.up);      lv_style_init(&S.down);    lv_style_init(&S.accent);
  lv_style_init(&S.hairline);lv_style_init(&S.dim);
  theme_on_apply(styles_apply);   // theme_set(idx) will call styles_apply
}
```

- [ ] **Step 1.3: Verify** — no standalone test (LVGL-coupled); compiles in Task 2's build. Proceed.

---

## Task 2: Screen interface + carousel + dot indicator

Pure index math (host-tested) + the scroll-snap pager with placeholder pages to prove navigation before real screens exist.

**Files:**
- Create: `src/ui/screen.h`, `src/ui/carousel_nav.h`, `src/ui/carousel.h`, `src/ui/carousel.cpp`, `test/test_carousel/test_main.cpp`

- [ ] **Step 2.1: Write `screen.h`**

```c
#pragma once
#include <lvgl.h>

typedef struct {
  const char* id;                       // eyebrow id: "HOME","MARKETS","LIMITS","CLAUDE","NOW","SETTINGS"
  lv_obj_t*  (*build)(lv_obj_t* page);  // build screen content into its page container
  void       (*update)(void);           // re-render from current DataStore snapshot (idempotent, read-only)
} screen_module_t;
```

- [ ] **Step 2.2: Write `carousel_nav.h` (pure index math, host-testable)**

```c
#pragma once
// Pure carousel index helpers (LVGL-free, host-tested). Clamp policy: NO wraparound.
static inline int carousel_clamp(int idx, int count) {
  if (count <= 0) return 0;
  if (idx < 0) return 0;
  if (idx >= count) return count - 1;
  return idx;
}
static inline int carousel_next(int idx, int count) { return carousel_clamp(idx + 1, count); }
static inline int carousel_prev(int idx, int count) { return carousel_clamp(idx - 1, count); }
// Nearest page index for a horizontal scroll offset, given page width and gap-free pages.
static inline int carousel_index_for_x(int scroll_x, int page_w, int count) {
  if (page_w <= 0) return 0;
  return carousel_clamp((scroll_x + page_w / 2) / page_w, count);
}
```

- [ ] **Step 2.3: Write the FAILING test (`test/test_carousel/test_main.cpp`)**

```cpp
#include <unity.h>
#include "ui/carousel_nav.h"

void setUp(void) {} void tearDown(void) {}

static void test_clamp_no_wrap(void) {
  TEST_ASSERT_EQUAL_INT(0, carousel_prev(0, 6));   // clamp at start
  TEST_ASSERT_EQUAL_INT(5, carousel_next(5, 6));   // clamp at end
  TEST_ASSERT_EQUAL_INT(3, carousel_next(2, 6));
  TEST_ASSERT_EQUAL_INT(1, carousel_prev(2, 6));
  TEST_ASSERT_EQUAL_INT(0, carousel_clamp(-3, 6));
  TEST_ASSERT_EQUAL_INT(5, carousel_clamp(99, 6));
  TEST_ASSERT_EQUAL_INT(0, carousel_clamp(0, 1));  // single screen edge
}
static void test_index_for_x(void) {
  struct { int x, w, count, expect; } c[] = {
    {0, 466, 6, 0}, {232, 466, 6, 0}, {233, 466, 6, 1},
    {466, 466, 6, 1}, {466*5, 466, 6, 5}, {466*99, 466, 6, 5},
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, carousel_index_for_x(c[i].x, c[i].w, c[i].count));
}
int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_clamp_no_wrap); RUN_TEST(test_index_for_x); return UNITY_END(); }
```

- [ ] **Step 2.4: Run host test, expect FAIL** (header absent): `~/.beacon-pio/bin/pio test -e native`. After 2.2 exists it PASSES.

- [ ] **Step 2.5: Write `carousel.h`**

```c
#pragma once
#include <lvgl.h>
#include "ui/screen.h"

// Builds the swipe carousel from a fixed module list + the dot indicator, applies the initial
// theme, and starts the ~500 ms visible-screen update timer. Call after styles_init() + lvgl up.
void carousel_init(void);
int  carousel_current(void);   // current page index
lv_obj_t* carousel_page(int idx);  // page container for module idx (for dev_seed long-press target)
```

- [ ] **Step 2.6: Write `carousel.cpp`** (placeholder pages first; real modules wired in Task 12)

```cpp
#include "ui/carousel.h"
#include "ui/carousel_nav.h"
#include "ui/styles.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"

// Module table is populated by the screen tasks; until then, a placeholder builder is used.
extern const screen_module_t* g_modules[];   // defined in carousel.cpp via the screens (Task 12)
extern int g_module_count;

static lv_obj_t* s_pager = nullptr;
static lv_obj_t* s_dots[8];
static lv_obj_t* s_pages[8];
static int s_count = 0;
static int s_current = 0;

static void set_dots(int active) {
  for (int i = 0; i < s_count; i++)
    lv_obj_set_style_bg_color(s_dots[i], i == active ? theme_active()->accent : theme_active()->line, 0);
}

static void scroll_cb(lv_event_t* e) {
  lv_obj_t* p = lv_event_get_target(e);
  int idx = carousel_index_for_x(lv_obj_get_scroll_x(p), SCREEN_W, s_count);
  if (idx != s_current) {
    s_current = idx;
    set_dots(idx);
    if (g_modules[idx]->update) g_modules[idx]->update();
  }
}

static void tick_cb(lv_timer_t*) {
  if (g_modules[s_current]->update) g_modules[s_current]->update();
}

void carousel_init(void) {
  s_count = g_module_count;

  s_pager = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(s_pager);
  lv_obj_set_size(s_pager, SCREEN_W, SCREEN_H);
  lv_obj_add_style(s_pager, &S.screen, 0);
  lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
  lv_obj_add_flag(s_pager, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(s_pager, 0, 0);
  lv_obj_set_style_pad_column(s_pager, 0, 0);
  lv_obj_add_event_cb(s_pager, scroll_cb, LV_EVENT_SCROLL_END, NULL);

  for (int i = 0; i < s_count; i++) {
    lv_obj_t* page = lv_obj_create(s_pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, SCREEN_W, SCREEN_H);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(page, 0, 0);
    s_pages[i] = g_modules[i]->build(page);
  }

  // Dot indicator on the top layer (does not scroll with pages), bottom arc-free band.
  lv_obj_t* bar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(bar);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(bar, 8, 0);
  lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET - 18));
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < s_count; i++) {
    lv_obj_t* d = lv_obj_create(bar);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 6, 6);
    lv_obj_set_style_radius(d, 3, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    s_dots[i] = d;
  }

  theme_set(0);             // populate styles + first restyle (Editorial)
  set_dots(0);
  if (g_modules[0]->update) g_modules[0]->update();
  lv_timer_create(tick_cb, 500, NULL);
}

int carousel_current(void) { return s_current; }
lv_obj_t* carousel_page(int idx) { return (idx >= 0 && idx < s_count) ? s_pages[idx] : nullptr; }
```

(Note: `g_modules`/`g_module_count` are defined in Task 12 once the six modules exist. To build+verify navigation BEFORE the screens, temporarily define a placeholder module array at the bottom of `carousel.cpp` guarded by `#ifndef BEACON_SCREENS_READY`; Task 12 removes the guard contents. For the first nav check, the placeholder build() makes a label with the module id centered.)

Temporary placeholder block (delete in Task 12):
```cpp
#ifndef BEACON_SCREENS_READY
static lv_obj_t* ph_build(lv_obj_t* page) {
  lv_obj_t* l = lv_label_create(page); lv_obj_add_style(l, &S.display, 0);
  lv_label_set_text(l, lv_obj_get_user_data(page) ? (const char*)lv_obj_get_user_data(page) : "PAGE");
  lv_obj_center(l); return page;
}
static void ph_update(void) {}
static const screen_module_t M_HOME{"HOME",ph_build,ph_update}, M_MK{"MARKETS",ph_build,ph_update},
  M_LIM{"LIMITS",ph_build,ph_update}, M_CL{"CLAUDE",ph_build,ph_update},
  M_NOW{"NOW",ph_build,ph_update}, M_SET{"SETTINGS",ph_build,ph_update};
const screen_module_t* g_modules[] = {&M_HOME,&M_MK,&M_LIM,&M_CL,&M_NOW,&M_SET};
int g_module_count = 6;
#endif
```

- [ ] **Step 2.7: Temporarily wire `main.cpp`** for a nav check: after `lvgl_port_begin()`, replace the `demo_screen` call with `styles_init(); carousel_init();` (includes: `ui/styles.h`, `ui/carousel.h`). Build + flash:
```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon -t upload && ~/.beacon-pio/bin/pio device monitor
```
**[HUMAN VISUAL CHECK]** Swipe left/right: pages slide with the finger and snap; six dots, the active one in accent; nav clamps at both ends (no wrap). Also note serial heap log.

- [ ] **Step 2.8: Commit checkpoint** —
```bash
git add firmware/beacon/src/ui/screen.h firmware/beacon/src/ui/carousel_nav.h firmware/beacon/src/ui/carousel.h firmware/beacon/src/ui/carousel.cpp firmware/beacon/src/ui/styles.h firmware/beacon/src/ui/styles.cpp firmware/beacon/test/test_carousel/test_main.cpp firmware/beacon/src/main.cpp
```
Message: `feat(p0c): scroll-snap carousel + dot indicator + shared theme styles`

---

## Task 3: State formatting helpers (`state_view.h`)

Pure, host-tested helpers that drive the per-state visual treatment. Screens call these + toggle `S.dim`.

**Files:**
- Create: `src/ui/state_view.h`, `test/test_state_view/test_main.cpp`

- [ ] **Step 3.1: Write `state_view.h` (pure, LVGL-free, host-tested)**

```c
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "core/screen_state.h"
#include "core/records.h"

// Compact age string: "now"/"12s"/"5m"/"3h"/"2d". UINT32_MAX => "--".
static inline void age_str(char* buf, size_t n, uint32_t age_s) {
  if (age_s == UINT32_MAX)      snprintf(buf, n, "--");
  else if (age_s < 5)           snprintf(buf, n, "now");
  else if (age_s < 60)          snprintf(buf, n, "%us", (unsigned)age_s);
  else if (age_s < 3600)        snprintf(buf, n, "%um", (unsigned)(age_s / 60));
  else if (age_s < 86400)       snprintf(buf, n, "%uh", (unsigned)(age_s / 3600));
  else                          snprintf(buf, n, "%ud", (unsigned)(age_s / 86400));
}

// Status-slot text for a record's state. Returns true if a non-live chip should show
// (caller swaps the live right-header for `buf`); false => LIVE, show the normal header.
static inline bool sv_status(char* buf, size_t n, const record_hdr_t* h, uint32_t now) {
  char age[8];
  switch (h->state) {
    case ST_LIVE:        return false;
    case ST_LOADING:     snprintf(buf, n, "..."); return true;
    case ST_STALE:       age_str(age, sizeof(age), record_age_s(h, now)); snprintf(buf, n, "STALE %s", age); return true;
    case ST_OFFLINE:     snprintf(buf, n, "OFFLINE"); return true;
    case ST_HUB_OFFLINE: snprintf(buf, n, "HUB OFFLINE"); return true;
    case ST_RECONNECTING:snprintf(buf, n, "RECONNECTING"); return true;
    case ST_ERROR:
      switch (h->err) {
        case ERR_RATE_LIMITED: snprintf(buf, n, "RATE LIMIT"); break;
        case ERR_TIMEOUT:      snprintf(buf, n, "TIMEOUT"); break;
        case ERR_NO_ROUTE:     snprintf(buf, n, "NO ROUTE"); break;
        default:               snprintf(buf, n, "ERR"); break;
      }
      return true;
    default:             snprintf(buf, n, "ERR"); return true;
  }
}

// Value should render as dim (stale/offline/error keep last value, dimmed).
static inline bool sv_dim(screen_state_t s) {
  return s == ST_STALE || s == ST_OFFLINE || s == ST_ERROR || s == ST_HUB_OFFLINE || s == ST_RECONNECTING;
}
// Value should render as placeholder dashes (no usable value yet).
static inline bool sv_placeholder(screen_state_t s) { return s == ST_LOADING; }
// Severe states color the chip with the down/alert color (vs ink_dim for stale age).
static inline bool sv_severe(screen_state_t s) {
  return s == ST_OFFLINE || s == ST_ERROR || s == ST_HUB_OFFLINE;
}
```

- [ ] **Step 3.2: Write the FAILING test (`test/test_state_view/test_main.cpp`)**

```cpp
#include <unity.h>
#include <string.h>
#include "ui/state_view.h"

void setUp(void) {} void tearDown(void) {}

static void test_age_str(void) {
  char b[8];
  age_str(b, sizeof(b), UINT32_MAX); TEST_ASSERT_EQUAL_STRING("--", b);
  age_str(b, sizeof(b), 0);   TEST_ASSERT_EQUAL_STRING("now", b);
  age_str(b, sizeof(b), 42);  TEST_ASSERT_EQUAL_STRING("42s", b);
  age_str(b, sizeof(b), 120); TEST_ASSERT_EQUAL_STRING("2m", b);
  age_str(b, sizeof(b), 7200);TEST_ASSERT_EQUAL_STRING("2h", b);
  age_str(b, sizeof(b), 172800);TEST_ASSERT_EQUAL_STRING("2d", b);
}
static void test_sv_status(void) {
  char b[16]; record_hdr_t h; h.last_updated = 1000; h.err = ERR_NONE;
  h.state = ST_LIVE;        TEST_ASSERT_FALSE(sv_status(b, sizeof(b), &h, 2000));
  h.state = ST_STALE;       TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 1000 + 120)); TEST_ASSERT_EQUAL_STRING("STALE 2m", b);
  h.state = ST_OFFLINE;     TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 2000)); TEST_ASSERT_EQUAL_STRING("OFFLINE", b);
  h.state = ST_HUB_OFFLINE; TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 2000)); TEST_ASSERT_EQUAL_STRING("HUB OFFLINE", b);
  h.state = ST_ERROR; h.err = ERR_RATE_LIMITED; TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 2000)); TEST_ASSERT_EQUAL_STRING("RATE LIMIT", b);
}
static void test_predicates(void) {
  TEST_ASSERT_TRUE(sv_dim(ST_STALE));   TEST_ASSERT_FALSE(sv_dim(ST_LIVE));
  TEST_ASSERT_TRUE(sv_placeholder(ST_LOADING)); TEST_ASSERT_FALSE(sv_placeholder(ST_STALE));
  TEST_ASSERT_TRUE(sv_severe(ST_OFFLINE)); TEST_ASSERT_FALSE(sv_severe(ST_STALE));
}
int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_age_str); RUN_TEST(test_sv_status); RUN_TEST(test_predicates); return UNITY_END(); }
```

- [ ] **Step 3.3: Run host test** — FAIL (header absent) then PASS after 3.1. `~/.beacon-pio/bin/pio test -e native`.

- [ ] **Step 3.4: Commit checkpoint** —
```bash
git add firmware/beacon/src/ui/state_view.h firmware/beacon/test/test_state_view/test_main.cpp
```
Message: `feat(p0c): host-tested screen-state formatting helpers`

---

## Task 4: Finance formatting (`fmt.h`)

Pure value/change formatting for the finance rows.

**Files:**
- Create: `src/ui/fmt.h`, `test/test_fmt/test_main.cpp`

- [ ] **Step 4.1: Write `fmt.h` (pure, host-tested)**

```c
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// Thousands-grouped value. Decimals: 0 for >=1000, else 2 (so 18026 => "18,026", 52.18 => "52.18").
static inline void fmt_value(char* buf, size_t n, double v) {
  int dec = (fabs(v) >= 1000.0) ? 0 : 2;
  char raw[32]; snprintf(raw, sizeof(raw), "%.*f", dec, v);
  // insert thousands separators into the integer part
  char* dot = strchr(raw, '.');
  int int_len = dot ? (int)(dot - raw) : (int)strlen(raw);
  int neg = raw[0] == '-';
  int digits = int_len - neg;
  char out[40]; int o = 0;
  for (int i = 0; i < int_len; i++) {
    if (i == neg) {} // no leading sep
    int pos = i - neg;                       // digit index within the number
    if (pos > 0 && (digits - pos) % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  if (dot) { while (*dot) out[o++] = *dot++; }
  out[o] = 0;
  snprintf(buf, n, "%s", out);
}

// Signed change: glyph (^ up / v down / - flat) + abs percent, e.g. "^ 0.12%%" -> "^ 0.12%".
// Returns: +1 up, -1 down, 0 flat (caller picks up/down/dim color).
static inline int fmt_change(char* buf, size_t n, double pct) {
  const char* g = pct > 0 ? "^" : (pct < 0 ? "v" : "-");
  snprintf(buf, n, "%s %.2f%%", g, fabs(pct));
  return pct > 0 ? 1 : (pct < 0 ? -1 : 0);
}
```
(`#include <string.h>` is needed for `strchr/strlen`; add it.)

- [ ] **Step 4.2: Write the FAILING test (`test/test_fmt/test_main.cpp`)**

```cpp
#include <unity.h>
#include <string.h>
#include "ui/fmt.h"

void setUp(void) {} void tearDown(void) {}

static void test_fmt_value(void) {
  char b[40];
  fmt_value(b, sizeof(b), 18026);   TEST_ASSERT_EQUAL_STRING("18,026", b);
  fmt_value(b, sizeof(b), 62392);   TEST_ASSERT_EQUAL_STRING("62,392", b);
  fmt_value(b, sizeof(b), 6012);    TEST_ASSERT_EQUAL_STRING("6,012", b);
  fmt_value(b, sizeof(b), 52.18);   TEST_ASSERT_EQUAL_STRING("52.18", b);
  fmt_value(b, sizeof(b), 5594);    TEST_ASSERT_EQUAL_STRING("5,594", b);
  fmt_value(b, sizeof(b), 1000000); TEST_ASSERT_EQUAL_STRING("1,000,000", b);
}
static void test_fmt_change(void) {
  char b[16];
  TEST_ASSERT_EQUAL_INT(1, fmt_change(b, sizeof(b), 0.12));  TEST_ASSERT_EQUAL_STRING("^ 0.12%", b);
  TEST_ASSERT_EQUAL_INT(-1, fmt_change(b, sizeof(b), -0.88));TEST_ASSERT_EQUAL_STRING("v 0.88%", b);
  TEST_ASSERT_EQUAL_INT(0, fmt_change(b, sizeof(b), 0.0));   TEST_ASSERT_EQUAL_STRING("- 0.00%", b);
}
int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_fmt_value); RUN_TEST(test_fmt_change); return UNITY_END(); }
```

- [ ] **Step 4.3: Run host test** — FAIL then PASS. `~/.beacon-pio/bin/pio test -e native`. (All host tests now green: smoke, records, config, hublink, datastore, theme, carousel, state_view, fmt.)

- [ ] **Step 4.4: Commit checkpoint** —
```bash
git add firmware/beacon/src/ui/fmt.h firmware/beacon/test/test_fmt/test_main.cpp
```
Message: `feat(p0c): host-tested finance value/change formatting`

---

## Tasks 5-10: The six screens

Each screen is a `screen_module_t`. Common build pattern: eyebrow (`S.eyebrow`) top-left, a status-slot label (`S.slot`) top-right, content per the editorial mockup; `update()` reads the DataStore snapshot, fills text, and applies state via `state_view.h` + toggling `S.dim` / chip text. Each screen file exposes `extern const screen_module_t <name>_module;`.

Shared helper used by every screen for the top-right slot (copy into each screen .cpp or a tiny shared inline) — implemented inline per screen for clarity:
```cpp
// set the right-header slot: live header text, or the state chip (dim for stale, down for severe)
static void slot_set(lv_obj_t* slot, const char* live, const record_hdr_t* h, uint32_t now) {
  char chip[16];
  if (sv_status(chip, sizeof(chip), h, now)) {
    lv_label_set_text(slot, chip);
    lv_obj_remove_style(slot, &S.dim, 0);
    if (sv_severe(h->state)) lv_obj_add_style(slot, &S.down, 0); else lv_obj_remove_style(slot, &S.down, 0);
  } else {
    lv_label_set_text(slot, live);
    lv_obj_remove_style(slot, &S.down, 0);
  }
}
```
`now` source: `static inline uint32_t now_s(){ return (uint32_t)(millis()/1000); }` (dev time base; P0-D swaps epoch). Put `now_s()` in a tiny shared `src/ui/screens/screen_common.h` plus the `slot_set` helper to avoid duplication.

### Task 5: `screen_common.h` + Home

**Files:**
- Create: `src/ui/screens/screen_common.h`, `src/ui/screens/screen_home.h`, `src/ui/screens/screen_home.cpp`

- [ ] **Step 5.1: Write `screen_common.h`**

```c
#pragma once
#include <lvgl.h>
#include <Arduino.h>
#include "ui/styles.h"
#include "ui/state_view.h"

static inline uint32_t now_s(void) { return (uint32_t)(millis() / 1000); }

static inline void slot_set(lv_obj_t* slot, const char* live, const record_hdr_t* h, uint32_t now) {
  char chip[16];
  if (sv_status(chip, sizeof(chip), h, now)) {
    lv_label_set_text(slot, chip);
    if (sv_severe(h->state)) lv_obj_add_style(slot, &S.down, 0); else lv_obj_remove_style(slot, &S.down, 0);
  } else {
    lv_label_set_text(slot, live);
    lv_obj_remove_style(slot, &S.down, 0);
  }
}

// dim or undim a value label per state (stale/offline keep value, dimmed; loading => placeholder text)
static inline void value_state(lv_obj_t* lbl, screen_state_t s) {
  if (sv_dim(s)) lv_obj_add_style(lbl, &S.dim, 0); else lv_obj_remove_style(lbl, &S.dim, 0);
}

// build the standard eyebrow ("BEACON / <id>") + return the top-right status slot label.
static inline lv_obj_t* build_header(lv_obj_t* page, const char* id) {
  lv_obj_t* eb = lv_label_create(page);
  lv_obj_add_style(eb, &S.eyebrow, 0);
  lv_label_set_text_fmt(eb, "BEACON / %s", id);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_t* slot = lv_label_create(page);
  lv_obj_add_style(slot, &S.slot, 0);
  lv_label_set_text(slot, "");
  lv_obj_align(slot, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);
  return slot;
}
```
(`#include "config/layout.h"` for SAFE_INSET; add it.)

- [ ] **Step 5.2: Write `screen_home.h`**
```c
#pragma once
#include "ui/screen.h"
extern const screen_module_t home_module;
```

- [ ] **Step 5.3: Write `screen_home.cpp`**
```cpp
#include "ui/screens/screen_home.h"
#include "ui/screens/screen_common.h"
#include "config/layout.h"
#include "core/datastore.h"

static lv_obj_t *s_slot, *s_clock, *s_date, *s_temp, *s_hum, *s_cond;

static lv_obj_t* build(lv_obj_t* page) {
  s_slot = build_header(page, "HOME");

  s_clock = lv_label_create(page); lv_obj_add_style(s_clock, &S.hero, 0);
  lv_label_set_text(s_clock, "--:--"); lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, SAFE_INSET, -30);

  s_date = lv_label_create(page); lv_obj_add_style(s_date, &S.slot, 0);
  lv_label_set_text(s_date, "--"); lv_obj_align(s_date, LV_ALIGN_LEFT_MID, SAFE_INSET, 30);

  lv_obj_t* rule = lv_obj_create(page); lv_obj_remove_style_all(rule);
  lv_obj_add_style(rule, &S.hairline, 0); lv_obj_set_size(rule, SCREEN_W - 2*SAFE_INSET, 1);
  lv_obj_align(rule, LV_ALIGN_LEFT_MID, SAFE_INSET, 60);

  s_temp = lv_label_create(page); lv_obj_add_style(s_temp, &S.display, 0);
  lv_obj_align(s_temp, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET - 24);
  s_hum = lv_label_create(page); lv_obj_add_style(s_hum, &S.display, 0);
  lv_obj_align(s_hum, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET + 150, -SAFE_INSET - 24);
  s_cond = lv_label_create(page); lv_obj_add_style(s_cond, &S.slot, 0);
  lv_obj_align(s_cond, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
  return page;
}

static void update(void) {
  // time service is P0-D: clock/date/week stay placeholders in C
  weather_rec_t w = ds_get_weather(); uint32_t now = now_s();
  slot_set(s_slot, "Wnn . --", &w.hdr, now);   // week/date placeholder until time service
  if (sv_placeholder(w.hdr.state)) { lv_label_set_text(s_temp, "--"); lv_label_set_text(s_hum, "--"); lv_label_set_text(s_cond, "--"); }
  else {
    lv_label_set_text_fmt(s_temp, "%.1f\xC2\xB0", w.temp_c);   // degree = U+00B0
    lv_label_set_text_fmt(s_hum, "%.0f%%", w.humidity_pct);
    lv_label_set_text(s_cond, "");   // condition label from WMO_MAP wired in P1; blank in C
  }
  value_state(s_temp, w.hdr.state); value_state(s_hum, w.hdr.state);
}

const screen_module_t home_module = {"HOME", build, update};
```

- [ ] **Step 5.4: Verify** — builds in Task 12's integration; no standalone flash. Stage:
```bash
git add firmware/beacon/src/ui/screens/screen_common.h firmware/beacon/src/ui/screens/screen_home.h firmware/beacon/src/ui/screens/screen_home.cpp
```

### Task 6: Finance (`MARKETS`)

**Files:** Create `src/ui/screens/screen_finance.{h,cpp}`

- [ ] **Step 6.1: `screen_finance.h`** — `extern const screen_module_t finance_module;`

- [ ] **Step 6.2: `screen_finance.cpp`**
```cpp
#include "ui/screens/screen_finance.h"
#include "ui/screens/screen_common.h"
#include "ui/fmt.h"
#include "config/layout.h"
#include "core/datastore.h"

#define MAX_ROWS 16
static lv_obj_t* s_slot;
static lv_obj_t* s_list;
static struct { lv_obj_t *id, *val, *chg; } s_row[MAX_ROWS];
static int s_rows;

static lv_obj_t* build(lv_obj_t* page) {
  s_slot = build_header(page, "MARKETS");
  s_list = lv_obj_create(page); lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2*SAFE_INSET, SCREEN_H - 2*SAFE_INSET - 30);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 30);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(s_list, 6, 0);

  s_rows = ds_get_finance_count(); if (s_rows > MAX_ROWS) s_rows = MAX_ROWS;
  for (int i = 0; i < s_rows; i++) {
    lv_obj_t* r = lv_obj_create(s_list); lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT); lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    s_row[i].id  = lv_label_create(r); lv_obj_add_style(s_row[i].id, &S.slot, 0);  lv_obj_align(s_row[i].id, LV_ALIGN_LEFT_MID, 0, 0);
    s_row[i].val = lv_label_create(r); lv_obj_add_style(s_row[i].val, &S.display, 0); lv_obj_align(s_row[i].val, LV_ALIGN_CENTER, 30, 0);
    s_row[i].chg = lv_label_create(r); lv_obj_add_style(s_row[i].chg, &S.slot, 0); lv_obj_align(s_row[i].chg, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  return page;
}

static void update(void) {
  uint32_t now = now_s();
  // overall slot reflects the worst non-live row's state; simple version: use slot 0 time-ish header
  bool any_nonlive = false; record_hdr_t worst; worst.state = ST_LIVE; worst.err = ERR_NONE; worst.last_updated = now;
  for (int i = 0; i < s_rows; i++) {
    finance_rec_t f = ds_get_finance(i);
    lv_label_set_text(s_row[i].id, f.id);
    if (sv_placeholder(f.hdr.state)) { lv_label_set_text(s_row[i].val, "--"); lv_label_set_text(s_row[i].chg, ""); }
    else {
      char v[40]; fmt_value(v, sizeof(v), f.value); lv_label_set_text(s_row[i].val, v);
      char c[16]; int dir = fmt_change(c, sizeof(c), f.change_pct); lv_label_set_text(s_row[i].chg, c);
      lv_obj_remove_style(s_row[i].chg, &S.up, 0); lv_obj_remove_style(s_row[i].chg, &S.down, 0);
      lv_obj_add_style(s_row[i].chg, dir >= 0 ? &S.up : &S.down, 0);
    }
    value_state(s_row[i].val, f.hdr.state);
    if (f.hdr.state != ST_LIVE) { any_nonlive = true; worst = f.hdr; }
  }
  slot_set(s_slot, "MARKETS", any_nonlive ? &worst : &worst, now);   // worst defaults LIVE => shows "MARKETS"
}

const screen_module_t finance_module = {"MARKETS", build, update};
```

- [ ] **Step 6.3: Stage** — `git add firmware/beacon/src/ui/screens/screen_finance.h firmware/beacon/src/ui/screens/screen_finance.cpp`

### Task 7: Usage (`LIMITS`)

**Files:** Create `src/ui/screens/screen_usage.{h,cpp}`

- [ ] **Step 7.1: `screen_usage.h`** — `extern const screen_module_t usage_module;`

- [ ] **Step 7.2: `screen_usage.cpp`** — CLAUDE + CODEX, each 5H/7D cell = big-% (`S.display`) + thin bar + `resets` label. A cell helper builds one window; `pct < 0` => `--` + no bar.
```cpp
#include "ui/screens/screen_usage.h"
#include "ui/screens/screen_common.h"
#include "config/layout.h"
#include "core/datastore.h"

static lv_obj_t* s_slot;
struct Cell { lv_obj_t *pct, *bar, *resets; };
static Cell s_c5, s_c7, s_x5, s_x7;   // claude 5h/7d, codex 5h/7d

static Cell make_cell(lv_obj_t* page, lv_align_t al, int dx, int dy, const char* tag) {
  Cell c;
  lv_obj_t* t = lv_label_create(page); lv_obj_add_style(t, &S.slot, 0);
  lv_label_set_text(t, tag); lv_obj_align(t, al, dx, dy);
  c.pct = lv_label_create(page); lv_obj_add_style(c.pct, &S.display, 0); lv_obj_align(c.pct, al, dx + 40, dy);
  c.bar = lv_bar_create(page); lv_obj_set_size(c.bar, 150, 4); lv_obj_align(c.bar, al, dx, dy + 36);
  lv_bar_set_range(c.bar, 0, 100);
  c.resets = lv_label_create(page); lv_obj_add_style(c.resets, &S.slot, 0); lv_obj_align(c.resets, al, dx, dy + 48);
  return c;
}

static void set_cell(Cell& c, const usage_window_t& w, const beacon_theme_t* th) {
  if (w.pct < 0) { lv_label_set_text(c.pct, "--"); lv_obj_add_flag(c.bar, LV_OBJ_FLAG_HIDDEN); }
  else {
    lv_label_set_text_fmt(c.pct, "%d%%", w.pct);
    lv_obj_clear_flag(c.bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(c.bar, w.pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(c.bar, th->line, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c.bar, th->accent, LV_PART_INDICATOR);
  }
  lv_label_set_text(c.resets, "resets --");   // reset-time formatting wired with time service (P1/D)
}

static lv_obj_t* build(lv_obj_t* page) {
  s_slot = build_header(page, "LIMITS");
  lv_obj_t* title = lv_label_create(page); lv_obj_add_style(title, &S.display, 0);
  lv_label_set_text(title, "AI USAGE"); lv_obj_align(title, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30);
  lv_obj_t* cl = lv_label_create(page); lv_obj_add_style(cl, &S.slot, 0); lv_label_set_text(cl, "CLAUDE");
  lv_obj_align(cl, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 80);
  lv_obj_t* cx = lv_label_create(page); lv_obj_add_style(cx, &S.slot, 0); lv_label_set_text(cx, "CODEX");
  lv_obj_align(cx, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 200);
  s_c5 = make_cell(page, LV_ALIGN_TOP_LEFT, SAFE_INSET,       SAFE_INSET + 110, "5H");
  s_c7 = make_cell(page, LV_ALIGN_TOP_LEFT, SAFE_INSET + 210, SAFE_INSET + 110, "7D");
  s_x5 = make_cell(page, LV_ALIGN_TOP_LEFT, SAFE_INSET,       SAFE_INSET + 230, "5H");
  s_x7 = make_cell(page, LV_ALIGN_TOP_LEFT, SAFE_INSET + 210, SAFE_INSET + 230, "7D");
  return page;
}

static void update(void) {
  usage_rec_t u = ds_get_usage(); const beacon_theme_t* th = theme_active(); uint32_t now = now_s();
  slot_set(s_slot, "POLL 30S", &u.hdr, now);
  set_cell(s_c5, u.claude.h5, th); set_cell(s_c7, u.claude.d7, th);
  set_cell(s_x5, u.codex.h5, th);  set_cell(s_x7, u.codex.d7, th);
}

const screen_module_t usage_module = {"LIMITS", build, update};
```
(`#include "ui/theme.h"` for `theme_active`; add it.)

- [ ] **Step 7.3: Stage** — `git add firmware/beacon/src/ui/screens/screen_usage.h firmware/beacon/src/ui/screens/screen_usage.cpp`

### Task 8: Buddy (`CLAUDE`)

**Files:** Create `src/ui/screens/screen_buddy.{h,cpp}`

- [ ] **Step 8.1: `screen_buddy.h`** — `extern const screen_module_t buddy_module;`

- [ ] **Step 8.2: `screen_buddy.cpp`** — status line + idle/prompt via `prompt.present`; Approve/Deny local stub; actions disabled on HUB_OFFLINE/RECONNECTING.
```cpp
#include "ui/screens/screen_buddy.h"
#include "ui/screens/screen_common.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "util/log.h"

static lv_obj_t *s_slot, *s_status, *s_kicker, *s_tool, *s_cmdbox, *s_cmd, *s_actions, *s_idle;

static void decide_cb(lv_event_t* e) {       // local stub; real hub round-trip is P2
  long approve = (long)lv_event_get_user_data(e);
  buddy_rec_t b = ds_get_buddy();
  LOGI("buddy decide id=%s approve=%ld (stub; hub round-trip is P2)", b.prompt.id, approve);
  b.prompt.present = false; ds_set_buddy(&b);   // clear locally
}

static lv_obj_t* mk_btn(lv_obj_t* parent, const char* txt, lv_align_t al, long approve) {
  lv_obj_t* b = lv_label_create(parent); lv_obj_add_style(b, &S.display, 0);
  if (approve) lv_obj_add_style(b, &S.accent, 0);
  lv_label_set_text(b, txt); lv_obj_align(b, al, al == LV_ALIGN_LEFT_MID ? SAFE_INSET : -SAFE_INSET, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 30);
  lv_obj_add_event_cb(b, decide_cb, LV_EVENT_CLICKED, (void*)approve);
  return b;
}

static lv_obj_t* build(lv_obj_t* page) {
  s_slot = build_header(page, "CLAUDE");
  s_status = lv_label_create(page); lv_obj_add_style(s_status, &S.slot, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 26);

  s_kicker = lv_label_create(page); lv_obj_add_style(s_kicker, &S.eyebrow, 0);
  lv_label_set_text(s_kicker, "PERMISSION -- APPROVE?"); lv_obj_align(s_kicker, LV_ALIGN_LEFT_MID, SAFE_INSET, -80);
  s_tool = lv_label_create(page); lv_obj_add_style(s_tool, &S.display, 0); lv_obj_align(s_tool, LV_ALIGN_LEFT_MID, SAFE_INSET, -30);
  s_cmdbox = lv_obj_create(page); lv_obj_remove_style_all(s_cmdbox);
  lv_obj_set_size(s_cmdbox, SCREEN_W - 2*SAFE_INSET, 56); lv_obj_align(s_cmdbox, LV_ALIGN_LEFT_MID, SAFE_INSET, 30);
  lv_obj_set_style_border_width(s_cmdbox, 1, 0); lv_obj_add_style(s_cmdbox, &S.dim, 0);
  s_cmd = lv_label_create(s_cmdbox); lv_obj_add_style(s_cmd, &S.body, 0); lv_obj_center(s_cmd);
  s_actions = lv_obj_create(page); lv_obj_remove_style_all(s_actions);
  lv_obj_set_size(s_actions, SCREEN_W, 40); lv_obj_align(s_actions, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);
  mk_btn(s_actions, "< DENY", LV_ALIGN_LEFT_MID, 0);
  mk_btn(s_actions, "APPROVE >", LV_ALIGN_RIGHT_MID, 1);

  s_idle = lv_label_create(page); lv_obj_add_style(s_idle, &S.slot, 0); lv_obj_center(s_idle);
  return page;
}

static void show_prompt(bool on) {
  lv_obj_t* p[] = {s_kicker, s_tool, s_cmdbox, s_actions};
  for (auto o : p) { if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
  if (on) lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  buddy_rec_t b = ds_get_buddy(); uint32_t now = now_s();
  slot_set(s_slot, "REQ --", &b.hdr, now);
  lv_label_set_text_fmt(s_status, "%u RUNNING . %u WAITING . %uK TOK . CTX %u%%",
    b.running, b.waiting, (unsigned)(b.tokens/1000), b.context_pct);
  bool disabled = (b.hdr.state == ST_HUB_OFFLINE || b.hdr.state == ST_RECONNECTING);
  if (b.prompt.present && !disabled) {
    show_prompt(true);
    lv_label_set_text(s_tool, b.prompt.tool);
    lv_label_set_text(s_cmd, b.prompt.hint);
  } else {
    show_prompt(false);
    if (disabled) lv_label_set_text(s_idle, "hub offline");
    else if (b.entry_count > 0) lv_label_set_text(s_idle, b.entries[0]);
    else lv_label_set_text(s_idle, "idle");
  }
}

const screen_module_t buddy_module = {"CLAUDE", build, update};
```

- [ ] **Step 8.3: Stage** — `git add firmware/beacon/src/ui/screens/screen_buddy.h firmware/beacon/src/ui/screens/screen_buddy.cpp`

### Task 9: Now-Playing (`NOW`)

**Files:** Create `src/ui/screens/screen_nowplaying.{h,cpp}`

- [ ] **Step 9.1: `screen_nowplaying.h`** — `extern const screen_module_t nowplaying_module;`

- [ ] **Step 9.2: `screen_nowplaying.cpp`** — title/artist/device + progress bar + play state; `has_device==false` => "no active device". Transport controls stubbed.
```cpp
#include "ui/screens/screen_nowplaying.h"
#include "ui/screens/screen_common.h"
#include "config/layout.h"
#include "core/datastore.h"

static lv_obj_t *s_slot, *s_title, *s_artist, *s_device, *s_prog, *s_state, *s_none;

static lv_obj_t* build(lv_obj_t* page) {
  s_slot = build_header(page, "NOW");
  s_title = lv_label_create(page); lv_obj_add_style(s_title, &S.display, 0);
  lv_obj_set_width(s_title, SCREEN_W - 2*SAFE_INSET); lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_align(s_title, LV_ALIGN_LEFT_MID, SAFE_INSET, -40);
  s_artist = lv_label_create(page); lv_obj_add_style(s_artist, &S.body, 0); lv_obj_add_style(s_artist, &S.dim, 0);
  lv_obj_align(s_artist, LV_ALIGN_LEFT_MID, SAFE_INSET, 0);
  s_prog = lv_bar_create(page); lv_obj_set_size(s_prog, SCREEN_W - 2*SAFE_INSET, 4);
  lv_obj_align(s_prog, LV_ALIGN_LEFT_MID, SAFE_INSET, 40); lv_bar_set_range(s_prog, 0, 1000);
  s_state = lv_label_create(page); lv_obj_add_style(s_state, &S.slot, 0); lv_obj_align(s_state, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);
  s_device = lv_label_create(page); lv_obj_add_style(s_device, &S.slot, 0); lv_obj_align(s_device, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
  s_none = lv_label_create(page); lv_obj_add_style(s_none, &S.slot, 0); lv_label_set_text(s_none, "no active device"); lv_obj_center(s_none);
  return page;
}

static void show_player(bool on) {
  lv_obj_t* p[] = {s_title, s_artist, s_prog, s_state, s_device};
  for (auto o : p) { if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
  if (on) lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_none, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  nowplaying_rec_t n = ds_get_nowplaying(); uint32_t now = now_s();
  slot_set(s_slot, "NOW", &n.hdr, now);
  if (!n.has_device || sv_placeholder(n.hdr.state)) { show_player(false); return; }
  show_player(true);
  lv_label_set_text(s_title, n.title);
  lv_label_set_text(s_artist, n.artist);
  lv_label_set_text(s_device, n.device);
  lv_label_set_text(s_state, n.playing ? "PLAYING" : "PAUSED");
  int32_t pos = (n.duration_ms > 0) ? (int32_t)((uint64_t)n.progress_ms * 1000 / n.duration_ms) : 0;
  lv_bar_set_value(s_prog, pos, LV_ANIM_OFF);
}

const screen_module_t nowplaying_module = {"NOW", build, update};
```

- [ ] **Step 9.3: Stage** — `git add firmware/beacon/src/ui/screens/screen_nowplaying.h firmware/beacon/src/ui/screens/screen_nowplaying.cpp`

### Task 10: Settings (`SETTINGS`) — theme + brightness live

**Files:** Create `src/ui/screens/screen_settings.{h,cpp}`

- [ ] **Step 10.1: `screen_settings.h`** — `extern const screen_module_t settings_module;`

- [ ] **Step 10.2: `screen_settings.cpp`** — rows; Theme row tap cycles theme; Brightness row tap cycles 40/60/80/100% via `display_brightness`. No persistence (P0-D).
```cpp
#include "ui/screens/screen_settings.h"
#include "ui/screens/screen_common.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "hal/display.h"

static lv_obj_t *s_theme_val, *s_bright_val, *s_tick_val;
static const uint8_t BRIGHT[] = {102, 153, 204, 255};   // 40/60/80/100%
static int s_bright_i = 2;

static lv_obj_t* row(lv_obj_t* page, const char* name, int y, bool tappable, lv_event_cb_t cb) {
  lv_obj_t* r = lv_obj_create(page); lv_obj_remove_style_all(r);
  lv_obj_set_size(r, SCREEN_W - 2*SAFE_INSET, 48); lv_obj_align(r, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30 + y);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* nm = lv_label_create(r); lv_obj_add_style(nm, &S.display, 0); lv_label_set_text(nm, name);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* val = lv_label_create(r); lv_obj_add_style(val, &S.slot, 0); lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
  if (tappable) { lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL); }
  lv_obj_t* rule = lv_obj_create(page); lv_obj_remove_style_all(rule); lv_obj_add_style(rule, &S.hairline, 0);
  lv_obj_set_size(rule, SCREEN_W - 2*SAFE_INSET, 1); lv_obj_align(rule, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30 + y + 48);
  return val;
}

static void theme_cb(lv_event_t*) {
  theme_set((theme_index() + 1) % THEME_COUNT);
  lv_label_set_text(s_theme_val, theme_active()->id);   // accent shown via add_style below
}
static void bright_cb(lv_event_t*) {
  s_bright_i = (s_bright_i + 1) % (int)(sizeof(BRIGHT)/sizeof(BRIGHT[0]));
  display_brightness(BRIGHT[s_bright_i]);
  lv_label_set_text_fmt(s_bright_val, "%d%%", (BRIGHT[s_bright_i] * 100 + 127) / 255);
}

static lv_obj_t* build(lv_obj_t* page) {
  build_header(page, "SETTINGS");
  lv_obj_t* wifi = row(page, "Wi-Fi", 0, false, NULL);            lv_label_set_text(wifi, "not set");
  s_bright_val   = row(page, "Brightness", 60, true, bright_cb);  lv_label_set_text(s_bright_val, "80%");
  s_theme_val    = row(page, "Theme", 120, true, theme_cb);       lv_obj_add_style(s_theme_val, &S.accent, 0);
  s_tick_val     = row(page, "Tickers", 180, false, NULL);
  lv_obj_t* sleep = row(page, "Sleep", 240, false, NULL);         lv_label_set_text(sleep, "5 min");
  lv_obj_t* about = row(page, "About", 300, false, NULL);         lv_label_set_text(about, ">");
  return page;
}

static void update(void) {
  if (theme_active()) lv_label_set_text(s_theme_val, theme_active()->id);
  lv_label_set_text_fmt(s_tick_val, "%d assets", ds_get_finance_count());
}

const screen_module_t settings_module = {"SETTINGS", build, update};
```

- [ ] **Step 10.3: Stage** — `git add firmware/beacon/src/ui/screens/screen_settings.h firmware/beacon/src/ui/screens/screen_settings.cpp`

---

## Task 11: Dev seeder + state-driver + Core-0 staleness ticker

**Files:** Create `src/ui/dev_seed.h`, `src/ui/dev_seed.cpp`

- [ ] **Step 11.1: `dev_seed.h`**
```c
#pragma once
// BEACON_DEV-only: seeds representative DataStore values so screens render populated,
// runs the Core-0 staleness ticker, and binds a long-press that cycles the visible
// screen's record through its plane's reachable states (Chunk-C verification harness).
void dev_seed_init(void);
```

- [ ] **Step 11.2: `dev_seed.cpp`**
```cpp
#include "ui/dev_seed.h"
#if BEACON_DEV
#include <Arduino.h>
#include <lvgl.h>
#include "core/datastore.h"
#include "core/stale.h"
#include "config/tickers.h"
#include "ui/carousel.h"
#include "util/log.h"

static uint32_t now_s(void) { return (uint32_t)(millis() / 1000); }

static void seed(void) {
  uint32_t now = now_s();
  weather_rec_t w; memset(&w, 0, sizeof(w)); w.temp_c = 31.8f; w.humidity_pct = 57; w.wmo_code = 2; w.hdr.last_updated = now;
  ds_set_weather(&w);
  const double vals[] = {18026, 62392, 6012, 19540, 52.18, 5594, 16800, 2400, 145, 7600};
  const double chg[]  = {0.12, 2.14, 0.41, 0.63, -0.88, -0.34, 0.05, 1.2, -0.4, 0.9};
  int n = ds_get_finance_count();
  for (int i = 0; i < n; i++) { finance_rec_t f; memset(&f, 0, sizeof(f)); f.value = vals[i % 10]; f.change_pct = chg[i % 10]; f.hdr.last_updated = now; ds_set_finance(i, &f); }
  usage_rec_t u; memset(&u, 0, sizeof(u)); u.claude.h5 = {24, now+7680}; u.claude.d7 = {24, now+400000};
  u.codex.h5 = {1, now+16860}; u.codex.d7 = {29, now+400000}; u.hdr.last_updated = now; ds_set_usage(&u);
  buddy_rec_t b; memset(&b, 0, sizeof(b)); b.running = 2; b.waiting = 1; b.tokens = 184502; b.context_pct = 42;
  b.prompt.present = true; strncpy(b.prompt.id, "A1B2", BUDDY_ID_LEN-1); strncpy(b.prompt.tool, "Bash", BUDDY_TOOL_LEN-1);
  strncpy(b.prompt.hint, "rm -rf /tmp/build-cache", BUDDY_HINT_LEN-1); b.hdr.last_updated = now; ds_set_buddy(&b);
  nowplaying_rec_t np; memset(&np, 0, sizeof(np)); np.has_device = true; np.playing = true;
  strncpy(np.title, "Midnight City", NP_TITLE_LEN-1); strncpy(np.artist, "M83", NP_ARTIST_LEN-1);
  strncpy(np.device, "Studio", NP_DEVICE_LEN-1); np.progress_ms = 60000; np.duration_ms = 240000; np.hdr.last_updated = now;
  ds_set_nowplaying(&np);
}

// Core-0 staleness ticker (DataStore sweeps are Core-0; P1's fetch task replaces this).
static void stale_task(void*) { for (;;) { ds_tick_staleness(now_s()); vTaskDelay(pdMS_TO_TICKS(1000)); } }

// Long-press on the active screen cycles its record through reachable states (dev fault injection).
static int s_phase = 0;
static void longpress_cb(lv_event_t*) {
  int scr = carousel_current(); s_phase = (s_phase + 1) % 4;
  // device-plane screens: home(0)=weather, finance(1)=finance slot0, nowplaying(4)=nowplaying
  // hub-plane: usage(2)/buddy(3) only LIVE<->HUB_OFFLINE
  screen_state_t dev[] = {ST_LIVE, ST_STALE, ST_OFFLINE, ST_ERROR};
  switch (scr) {
    case 0: if (s_phase==0) { seed(); } else ds_set_state_weather(dev[s_phase], ERR_RATE_LIMITED); break;
    case 1: if (s_phase==0) { seed(); } else ds_set_state_finance(0, dev[s_phase], ERR_TIMEOUT); break;
    case 4: if (s_phase==0) { seed(); } else ds_set_state_nowplaying(dev[s_phase], ERR_NO_ROUTE); break;
    case 2: case 3: if (s_phase % 2) ds_set_hub_offline(); else seed(); break;
    default: break;
  }
  LOGI("dev: screen=%d phase=%d", scr, s_phase);
}

void dev_seed_init(void) {
  seed();
  xTaskCreatePinnedToCore(stale_task, "stale", 2048, nullptr, 1, nullptr, 0);   // Core 0
  lv_obj_add_event_cb(lv_scr_act(), longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
}
#else
void dev_seed_init(void) {}
#endif
```

- [ ] **Step 11.3: Stage** — `git add firmware/beacon/src/ui/dev_seed.h firmware/beacon/src/ui/dev_seed.cpp`

---

## Task 12: Wire the module table + main.cpp; remove dead screens

**Files:** Modify `src/ui/carousel.cpp`, `src/main.cpp`; delete `src/ui/test_screen.{h,cpp}`, `src/ui/demo_screen.{h,cpp}`

- [ ] **Step 12.1: Replace the placeholder block in `carousel.cpp`** — delete the `#ifndef BEACON_SCREENS_READY` block (Step 2.6) and add the real module table near the top (after includes):
```cpp
#include "ui/screens/screen_home.h"
#include "ui/screens/screen_finance.h"
#include "ui/screens/screen_usage.h"
#include "ui/screens/screen_buddy.h"
#include "ui/screens/screen_nowplaying.h"
#include "ui/screens/screen_settings.h"

const screen_module_t* g_modules[] = {
  &home_module, &finance_module, &usage_module, &buddy_module, &nowplaying_module, &settings_module,
};
int g_module_count = 6;
```
(Remove the `extern const screen_module_t* g_modules[];` / `extern int g_module_count;` forward decls — they are now definitions.)

- [ ] **Step 12.2: Final `main.cpp`** — includes carousel + styles + dev_seed; drop demo/test screen:
```cpp
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"
#include "ui/styles.h"
#include "ui/carousel.h"
#include "ui/dev_seed.h"

void setup() {
  Serial.begin(115200); delay(300);
  LOGI("boot - core=%s", ESP_ARDUINO_VERSION_STR);
  enableLoopWDT();
  if (!power_begin())   { LOGE("halt: power");   return; }
  delay(120);
  if (!display_begin()) { LOGE("halt: display"); return; }
  touch_begin();
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { LOGE("halt: no PSRAM"); return; }
  LOGI("psram total=%u free=%u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (!lvgl_port_begin()) { LOGE("halt: lvgl"); return; }
  styles_init();
  carousel_init();
  dev_seed_init();
  LOGI("setup done; swipe to navigate");
}

void loop() { lvgl_port_tick(); delay(5); }
```

- [ ] **Step 12.3: Delete superseded files**
```bash
git rm firmware/beacon/src/ui/test_screen.h firmware/beacon/src/ui/test_screen.cpp firmware/beacon/src/ui/demo_screen.h firmware/beacon/src/ui/demo_screen.cpp
```

- [ ] **Step 12.4: Build + host tests**
```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio test -e native && ~/.beacon-pio/bin/pio run -e beacon
```
Expected: all host tests PASS; device build links clean. Note Flash% (fonts + screens).

- [ ] **Step 12.5: Commit checkpoint** —
```bash
git add -A firmware/beacon
```
Message: `feat(p0c): wire six screens into the carousel; remove demo/test screens`

---

## Task 13: Integration verification + heap gate

- [ ] **Step 13.1: Flash + monitor**
```bash
cd firmware/beacon && ~/.beacon-pio/bin/pio run -e beacon -t upload && ~/.beacon-pio/bin/pio device monitor
```

- [ ] **Step 13.2: Add a heap-gate log** — in `lvgl_port_tick()` or a 5 s timer, log min internal heap + largest DMA block + min PSRAM + largest PSRAM block. Simplest: add to `dev_seed`'s `stale_task` loop every 5th tick:
```cpp
    static int k = 0; if (++k % 5 == 0)
      LOGI("heap: int_free=%u int_dma_blk=%u psram_free=%u psram_blk=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
```
(add `#include <esp_heap_caps.h>` to dev_seed.cpp)

- [ ] **Step 13.3: [HUMAN VISUAL CHECK]** — verify on the panel:
  - swipe through all six; pages slide with the finger + snap; six dots track; clamps at ends.
  - each screen matches its editorial mockup (Home/MARKETS/LIMITS/CLAUDE/NOW/SETTINGS) with seeded data.
  - Settings: tap Theme cycles all 7 themes live (every screen restyles); tap Brightness steps 40/60/80/100%.
  - long-press cycles the visible screen through its states (home/finance/nowplaying: live=>stale=>offline=>error; usage/buddy: live<=>hub-offline) with correct chip + dimming.
  - finance vertical scroll (if >6 tickers) works without breaking horizontal paging.

- [ ] **Step 13.4: Self-verify heap gate (serial)** — confirm `int_free` holds >= 60 KB with margin and PSRAM has comfortable headroom across swipes + a 7-theme cycle; no LVGL alloc-fail logs. Record the numbers.

- [ ] **Step 13.5: Commit checkpoint** —
```bash
git add -A firmware/beacon
```
Message: `feat(p0c): integration + heap gate instrumentation`

---

## Self-review

**Spec coverage:** carousel + indicator (Task 2) FR-PLAT-2; safe area (every screen uses SAFE_INSET) FR-PLAT-4; six state-aware screens (Tasks 5-10) FR-STATE-1/2; theme live switch on real screens (Task 1 + Settings) FR-THEME-3; Settings theme+brightness live (Task 10) FR-SET-2/3; PSRAM pool + heap gate (Tasks 0/13) NFR; UI-never-crashes via state-driver fault injection (Task 11/13) FR-STATE-3; dev verification harness (Task 11). All spec sections map to a task.

**Placeholder scan:** no TBD/TODO; every code step is complete. The only intentional runtime placeholders are the time-dependent fields (clock/date/reset-time/condition) explicitly deferred to P0-D/P1 per the spec, rendered as `--`.

**Type consistency:** `screen_module_t {id, build, update}` used identically across all screens + carousel; `g_modules`/`g_module_count` defined once (Task 12); shared `app_styles_t S`; `now_s()` defined in `screen_common.h` (screens) and locally in `dev_seed.cpp`; state helpers (`sv_status/sv_dim/sv_placeholder/sv_severe`, `age_str`) used consistently; DataStore getters/setters match `datastore.h` exactly (`ds_set_state_weather/finance/nowplaying`, `ds_set_hub_offline`, no usage/buddy state setters used).

**Known on-device tuning expected:** exact pixel offsets in screen layouts are first-pass; the [HUMAN VISUAL CHECK] at 13.3 is where they get nudged to match the mockups. Functionality (nav, state, theme, brightness, heap) is the gate, not pixel-perfection.
