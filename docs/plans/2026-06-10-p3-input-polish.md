# P3 — Input Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable idle dim/sleep/wake layer (all screens, configurable timers incl. "Never"), swipe-down pull-to-refresh on data screens, and IMU motion gestures (raise = wake, shake = dismiss overlay) to the Beacon firmware.

**Architecture:** Pure, host-tested decision cores (`core/idle`, `core/imu_detect`) hold the logic; thin HAL/UI glue feeds them inputs and applies effects. Idle uses LVGL's built-in inactivity timer as the single activity clock (touch resets it for free; IMU resets it via `lv_disp_trigger_activity`). Dim/Sleep timeouts persist in NVS and are edited through a generic duration picker modal modeled on the existing `theme_panel`.

**Tech Stack:** C/C++ (Arduino-ESP32 core 3.3.5, ESP-IDF 5.5.1), LVGL 8.4, SensorLib 0.3.3 (QMI8658 IMU), PlatformIO (`env:beacon` device, `env:native` host tests with Unity).

---

## Decisions locked (from requirements discussion)

- Dim, Sleep, wake-on-touch apply to **all** screens uniformly.
- Two **independent** settings: `Dim after` and `Sleep after`, each a tap-to-open list picker (same UX as the Theme picker), with a **Never** option (#4).
- Swipe-down = **immediate refresh** on data screens (Finance + Home weather). AI Usage refresh is **out of scope** (would need a new hub BLE RX command — flagged, not built).
- **Long-press: dropped** (swipe-down took over refresh; FR-PLAT-5 long-press is SHOULD, deferred).
- Idle/IMU logic must be **reusable** (#2) → pure cores + host tests.

## Scope note: FR-PLAT-7 was deferred from P0

The base idle dim/sleep machine does not exist yet (`main.cpp` only restores brightness at boot). This plan delivers that base **and** the P3 IMU-wake on top of it. That is intentional and in-scope here.

## File map

| File | Responsibility | New/Mod |
|---|---|---|
| `firmware/src/core/idle.h` / `idle.cpp` | Pure `idle_eval(inactive_ms, dim_ms, sleep_ms) -> phase` | New |
| `firmware/test/test_idle/test_main.cpp` | Table tests for idle_eval | New |
| `firmware/src/core/imu_detect.h` / `imu_detect.cpp` | Pure gesture detector (accel samples -> RAISE/SHAKE events) | New |
| `firmware/test/test_imu_detect/test_main.cpp` | Table tests for detector | New |
| `firmware/src/core/nvs.h` / `nvs.cpp` | Add `dim_idx` / `slp_idx` typed getters/setters | Mod |
| `firmware/src/ui/durations.h` | Shared duration preset table | New |
| `firmware/src/ui/duration_panel.h` / `duration_panel.cpp` | Generic list-picker modal (reuses theme_panel pattern) | New |
| `firmware/src/ui/settings_power_rows.h` / `.cpp` | Shared Dim/Sleep open-callbacks + label fmt | New |
| `firmware/src/ui/screens/views/settings_*.cpp` (7) | Add Dim + Sleep rows | Mod |
| `firmware/src/ui/idle_glue.h` / `idle_glue.cpp` | Drive brightness/dim/sleep from idle_eval; wake handling | New |
| `firmware/src/ui/lvgl_port.cpp` | Swallow the wake-from-sleep touch | Mod |
| `firmware/src/main.cpp` | Init idle config + call `idle_service()` in loop | Mod |
| `firmware/src/core/fetch_task.h` / `fetch_task.cpp` | `fetch_task_refresh_all()` | Mod |
| `firmware/src/ui/carousel.cpp` | Swipe-down gesture -> refresh on Home/Finance | Mod |
| `firmware/src/hal/imu.h` / `imu.cpp` | QMI8658 I2C bring-up + sample read | New |
| `firmware/src/ui/theme_panel.{h,cpp}`, `wifi_panel.{h,cpp}` | Expose `*_close()` for shake-dismiss | Mod |
| `firmware/src/ui/overlays.h` / `overlays.cpp` | `ui_dismiss_top_overlay()` dispatcher | New |
| `firmware/platformio.ini` | Add new pure TUs to `env:native` `build_src_filter` | Mod |

---

## Task 1: Pure idle state machine (`core/idle`)

**Files:**
- Create: `firmware/src/core/idle.h`, `firmware/src/core/idle.cpp`
- Test: `firmware/test/test_idle/test_main.cpp`

- [ ] **Step 1: Write the failing test**

`firmware/test/test_idle/test_main.cpp`:

```cpp
#include <unity.h>
#include "core/idle.h"

void setUp(void) {} void tearDown(void) {}

// idle_eval maps inactivity to a phase. ms == 0 for either threshold means "never".
static void test_eval(void) {
  const uint32_t D = 60000;   // dim after 1 min
  const uint32_t S = 300000;  // sleep after 5 min
  struct { uint32_t inact, dim, slp; idle_phase_t expect; } c[] = {
    {0,        D, S, IDLE_ACTIVE},
    {59999,    D, S, IDLE_ACTIVE},
    {60000,    D, S, IDLE_DIM},     // exactly at dim threshold
    {299999,   D, S, IDLE_DIM},
    {300000,   D, S, IDLE_SLEEP},   // exactly at sleep threshold
    {999999,   D, S, IDLE_SLEEP},
    {999999,   0, S, IDLE_SLEEP},   // dim=never: jump straight to sleep at S
    {120000,   0, S, IDLE_ACTIVE},  // dim=never, below sleep => stay active (no dim stage)
    {999999,   D, 0, IDLE_DIM},     // sleep=never: dim but never sleep
    {999999,   0, 0, IDLE_ACTIVE},  // both never: always active
    {400000,   S, D, IDLE_SLEEP},   // misordered (dim>=sleep): sleep still wins past S
  };
  for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); i++)
    TEST_ASSERT_EQUAL_INT(c[i].expect, idle_eval(c[i].inact, c[i].dim, c[i].slp));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_eval);
  return UNITY_END();
}
```

- [ ] **Step 2: Create the header**

`firmware/src/core/idle.h`:

```cpp
#pragma once
#include <stdint.h>

// Pure idle decision (FR-PLAT-7). No Arduino/LVGL — host-tested. The caller owns the
// activity clock (LVGL inactivity) and the effects (brightness/sleep). dim_ms/sleep_ms == 0
// means that stage is disabled ("Never"). Sleep takes precedence over dim past its threshold.
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { IDLE_ACTIVE = 0, IDLE_DIM = 1, IDLE_SLEEP = 2 } idle_phase_t;

idle_phase_t idle_eval(uint32_t inactive_ms, uint32_t dim_ms, uint32_t sleep_ms);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Implement**

`firmware/src/core/idle.cpp`:

```cpp
#include "core/idle.h"

idle_phase_t idle_eval(uint32_t inactive_ms, uint32_t dim_ms, uint32_t sleep_ms) {
  if (sleep_ms != 0 && inactive_ms >= sleep_ms) return IDLE_SLEEP;
  if (dim_ms   != 0 && inactive_ms >= dim_ms)   return IDLE_DIM;
  return IDLE_ACTIVE;
}
```

- [ ] **Step 4: Register the TU for host tests**

In `firmware/platformio.ini`, append `+<core/idle.cpp>` to the `[env:native]` `build_src_filter` line.

- [ ] **Step 5: Run the test**

Run: `cd firmware && pio test -e native -f test_idle`
Expected: PASS (1 test).

- [ ] **Step 6: Commit**

```bash
git add firmware/src/core/idle.h firmware/src/core/idle.cpp firmware/test/test_idle/test_main.cpp firmware/platformio.ini
git commit -m "feat(idle): pure idle dim/sleep decision core + host tests"
```

---

## Task 2: NVS persistence for Dim/Sleep indices

**Files:**
- Modify: `firmware/src/core/nvs.h`, `firmware/src/core/nvs.cpp`

Indices into the shared preset table (Task 3) fit in a byte; store the index, not raw ms.

- [ ] **Step 1: Declare getters/setters**

In `firmware/src/core/nvs.h`, after the `nvs_get_theme` line, add:

```cpp
uint8_t nvs_get_dim_idx(uint8_t def);    void nvs_set_dim_idx(uint8_t v);
uint8_t nvs_get_sleep_idx(uint8_t def);  void nvs_set_sleep_idx(uint8_t v);
```

- [ ] **Step 2: Implement over the generic byte store**

In `firmware/src/core/nvs.cpp`, mirror the existing `nvs_get_theme`/`nvs_set_theme` pair (they delegate to `nvs_get_byte`/`nvs_set_byte`). Add:

```cpp
uint8_t nvs_get_dim_idx(uint8_t def)    { return nvs_get_byte("dim_idx", def); }
void    nvs_set_dim_idx(uint8_t v)      { nvs_set_byte("dim_idx", v); }
uint8_t nvs_get_sleep_idx(uint8_t def)  { return nvs_get_byte("slp_idx", def); }
void    nvs_set_sleep_idx(uint8_t v)    { nvs_set_byte("slp_idx", v); }
```

(Match the exact placement/style of the existing typed wrappers in the file.)

- [ ] **Step 3: Build to verify it compiles**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -5`
Expected: build succeeds (no upload needed).

- [ ] **Step 4: Commit**

```bash
git add firmware/src/core/nvs.h firmware/src/core/nvs.cpp
git commit -m "feat(nvs): persist dim/sleep timeout indices"
```

---

## Task 3: Shared duration presets

**Files:**
- Create: `firmware/src/ui/durations.h`

- [ ] **Step 1: Create the table**

`firmware/src/ui/durations.h`:

```cpp
#pragma once
#include <stdint.h>

// Shared dim/sleep timeout presets (FR-PLAT-7 / FR-SET-5). ms == 0 => "Never".
// Indices are persisted in NVS (nvs_get/set_dim_idx, _sleep_idx). Append-only:
// never reorder, or stored indices shift meaning.
typedef struct { const char* label; uint32_t ms; } duration_opt_t;

static const duration_opt_t DURATIONS[] = {
  { "15 sec", 15000  },
  { "30 sec", 30000  },
  { "1 min",  60000  },
  { "2 min",  120000 },
  { "5 min",  300000 },
  { "10 min", 600000 },
  { "Never",  0      },
};
#define DURATION_COUNT ((uint8_t)(sizeof(DURATIONS) / sizeof(DURATIONS[0])))
#define DURATION_DEFAULT_DIM   2   // 1 min
#define DURATION_DEFAULT_SLEEP 4   // 5 min
```

- [ ] **Step 2: Commit**

```bash
git add firmware/src/ui/durations.h
git commit -m "feat(ui): shared dim/sleep duration presets"
```

---

## Task 4: Generic duration picker modal

**Files:**
- Create: `firmware/src/ui/duration_panel.h`, `firmware/src/ui/duration_panel.cpp`

Reuses the exact structure of `firmware/src/ui/theme_panel.cpp` (modal on `lv_layer_top`, suspends carousel swipe, deferred apply via `lv_async_call`), generalized to any label/value/setter.

- [ ] **Step 1: Header**

`firmware/src/ui/duration_panel.h`:

```cpp
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Generic list picker over DURATIONS (durations.h), modeled on theme_panel. Opened from a
// settings row; lists every preset (current marked), tapping one applies via on_pick and closes.
// Suspends carousel swipe while open; restored on close. One instance.
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*duration_pick_cb)(uint8_t idx);

void duration_panel_open(const char* title, uint8_t current, duration_pick_cb on_pick);
bool duration_panel_is_open(void);
void duration_panel_close(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implementation**

`firmware/src/ui/duration_panel.cpp`:

```cpp
#include "ui/duration_panel.h"
#include "ui/durations.h"
#include "ui/theme.h"
#include "ui/carousel.h"
#include "config/layout.h"
#include <lvgl.h>

static lv_obj_t*        s_root  = nullptr;
static duration_pick_cb s_cb    = nullptr;
static uint8_t          s_pick  = 0;

static void mklabel(lv_obj_t* o, const lv_font_t* f, lv_color_t c) {
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_set_style_text_color(o, c, 0);
  lv_obj_set_style_text_letter_space(o, 2, 0);
}

void duration_panel_close(void) {
  if (!s_root) return;
  lv_obj_del(s_root);
  s_root = nullptr;
  s_cb = nullptr;
  carousel_set_swipe_enabled(true);
}

// Deferred: applying from inside the tapped row's own event while we also free s_root is unsafe.
static void apply_pick(void*) {
  duration_pick_cb cb = s_cb;
  uint8_t idx = s_pick;
  duration_panel_close();
  if (cb) cb(idx);
}

static void back_cb(lv_event_t*) { duration_panel_close(); }
static void row_cb(lv_event_t* e) {
  s_pick = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  lv_async_call(apply_pick, NULL);
}

void duration_panel_open(const char* title, uint8_t current, duration_pick_cb on_pick) {
  if (s_root) return;
  const beacon_theme_t* t = theme_active();
  s_cb = on_pick;
  carousel_set_swipe_enabled(false);

  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
  lv_obj_center(s_root);
  lv_obj_set_style_bg_color(s_root, t->bg, 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* back = lv_label_create(s_root); mklabel(back, t->f_body, t->ink_dim);
  lv_label_set_text(back, "< back"); lv_obj_align(back, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(back, 24);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* ttl = lv_label_create(s_root); mklabel(ttl, t->f_body, t->ink_dim);
  lv_label_set_text(ttl, title); lv_obj_align(ttl, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* list = lv_obj_create(s_root);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(list, 2, 0);

  for (uint8_t i = 0; i < DURATION_COUNT; i++) {
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, i == current ? t->accent : t->line, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t* lbl = lv_label_create(row);
    mklabel(lbl, t->f_display, i == current ? t->accent : t->ink);
    lv_label_set_text(lbl, DURATIONS[i].label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
  }
}

bool duration_panel_is_open(void) { return s_root != nullptr; }
```

- [ ] **Step 3: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -5`
Expected: build succeeds (the panel is unused until Task 5, but must compile).

- [ ] **Step 4: Commit**

```bash
git add firmware/src/ui/duration_panel.h firmware/src/ui/duration_panel.cpp
git commit -m "feat(ui): generic duration picker modal"
```

---

## Task 5: Shared Dim/Sleep settings logic

**Files:**
- Create: `firmware/src/ui/settings_power_rows.h`, `firmware/src/ui/settings_power_rows.cpp`

One home for the open-callbacks, label formatting, and persistence so the 7 bespoke settings views stay thin and DRY.

- [ ] **Step 1: Header**

`firmware/src/ui/settings_power_rows.h`:

```cpp
#pragma once
#include <stddef.h>

// Shared Dim/Sleep settings behaviour (used by every settings_*.cpp view). Opens the duration
// picker, persists the choice, and re-applies the live idle config. Views own only the rows.
#ifdef __cplusplus
extern "C" {
#endif

void settings_power_open_dim(void);     // wire as a row's LV_EVENT_CLICKED handler body
void settings_power_open_sleep(void);
void settings_power_dim_label(char* out, size_t cap);    // current dim preset label
void settings_power_sleep_label(char* out, size_t cap);  // current sleep preset label

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implementation**

`firmware/src/ui/settings_power_rows.cpp`:

```cpp
#include "ui/settings_power_rows.h"
#include "ui/duration_panel.h"
#include "ui/durations.h"
#include "ui/idle_glue.h"
#include "core/nvs.h"
#include <string.h>

static void copy_label(char* out, size_t cap, uint8_t idx) {
  if (idx >= DURATION_COUNT) idx = 0;
  strncpy(out, DURATIONS[idx].label, cap - 1);
  out[cap - 1] = 0;
}

static void on_dim(uint8_t idx)   { nvs_set_dim_idx(idx);   idle_apply_config_from_nvs(); }
static void on_sleep(uint8_t idx) { nvs_set_sleep_idx(idx); idle_apply_config_from_nvs(); }

void settings_power_open_dim(void) {
  duration_panel_open("DIM AFTER", nvs_get_dim_idx(DURATION_DEFAULT_DIM), on_dim);
}
void settings_power_open_sleep(void) {
  duration_panel_open("SLEEP AFTER", nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP), on_sleep);
}
void settings_power_dim_label(char* out, size_t cap)   { copy_label(out, cap, nvs_get_dim_idx(DURATION_DEFAULT_DIM)); }
void settings_power_sleep_label(char* out, size_t cap) { copy_label(out, cap, nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP)); }
```

(`idle_glue.h` / `idle_apply_config_from_nvs` are created in Task 7. This file will not link until then; that is fine — it compiles against the header. If executing strictly task-by-task, build at the end of Task 7.)

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui/settings_power_rows.h firmware/src/ui/settings_power_rows.cpp
git commit -m "feat(ui): shared dim/sleep settings callbacks"
```

---

## Task 6: Wire Dim + Sleep rows into all 7 settings views

**Files (each Modify):**
`firmware/src/ui/screens/views/settings_editorial.cpp`, `settings_hud.cpp`, `settings_blueprint.cpp`, `settings_led.cpp`, `settings_oscilloscope.cpp`, `settings_analog.cpp`, `settings_calm.cpp`

Each view shares the same shape: a `build(page)` that lays rows via a local `row(...)` helper, and an `update()` that refreshes value labels. The existing hardcoded stub `Sleep` row (`"5 min"`) is **replaced** by two live rows: `Dim` and `Sleep`.

The reference below is for `settings_editorial.cpp` (its `row(page, name, y, cb)` returns the value label and takes a y-offset). The other six views use their own row helper signatures and y math — apply the identical logic, adapting only the row-construction call to each file's helper.

- [ ] **Step 1 (editorial): add include + value-label pointers**

In `firmware/src/ui/screens/views/settings_editorial.cpp`, add the include near the other `ui/` includes:

```cpp
#include "ui/settings_power_rows.h"
```

Add two file-scope label pointers next to the existing `s_*_val` declarations:

```cpp
static lv_obj_t *s_dim_val, *s_sleep_val;
```

- [ ] **Step 2 (editorial): add row callbacks**

Next to `theme_cb`/`wifi_open_cb`:

```cpp
static void dim_cb(lv_event_t*)   { settings_power_open_dim(); }
static void sleep_cb(lv_event_t*) { settings_power_open_sleep(); }
```

- [ ] **Step 3 (editorial): replace the stub Sleep row in build()**

Find:

```cpp
  lv_obj_t* slp  = row(page, "Sleep", 250, NULL);           lv_label_set_text(slp, "5 min");
  lv_obj_t* abt  = row(page, "About", 300, NULL);           lv_label_set_text(abt, ">");
```

Replace with (Tickers stays at 200; insert Dim at 250, Sleep at 300, push About to 350):

```cpp
  s_dim_val      = row(page, "Dim", 250, dim_cb);
  s_sleep_val    = row(page, "Sleep", 300, sleep_cb);
  lv_obj_t* abt  = row(page, "About", 350, NULL);           lv_label_set_text(abt, ">");
```

- [ ] **Step 4 (editorial): set labels in update()**

At the end of `update()`:

```cpp
  char db[12], sb[12];
  settings_power_dim_label(db, sizeof(db));   lv_label_set_text(s_dim_val, db);
  settings_power_sleep_label(sb, sizeof(sb)); lv_label_set_text(s_sleep_val, sb);
```

- [ ] **Step 5: repeat Steps 1-4 for the other six settings views**

For each of `settings_hud.cpp`, `settings_blueprint.cpp`, `settings_led.cpp`, `settings_oscilloscope.cpp`, `settings_analog.cpp`, `settings_calm.cpp`:
1. `#include "ui/settings_power_rows.h"`.
2. Add `static lv_obj_t *s_dim_val, *s_sleep_val;` (use that view's existing naming convention).
3. Add `dim_cb` / `sleep_cb` (bodies identical to Step 2).
4. Locate that view's existing Sleep/idle row (grep `Sleep` in the file) and replace it with a `Dim` row + `Sleep` row using the view's own row helper and y-rhythm; bind `dim_cb` / `sleep_cb`.
5. In `update()`, set both labels via `settings_power_dim_label` / `settings_power_sleep_label` (identical to Step 4).

Verify each before moving on: `cd firmware && grep -c settings_power firmware/src/ui/screens/views/settings_*.cpp` should print `4` for every file (1 include + 2 opens + 1 label pair spans both helpers — confirm at least the include and one open per file).

- [ ] **Step 6: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -8`
Expected: succeeds **after Task 7 lands** (`idle_glue.h` dependency). If building now, stub `idle_apply_config_from_nvs()` is not yet defined — do Task 7 next, then build.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/ui/screens/views/settings_*.cpp
git commit -m "feat(settings): live Dim + Sleep timeout rows across all 7 themes"
```

---

## Task 7: Idle glue — drive brightness/dim/sleep + wake

**Files:**
- Create: `firmware/src/ui/idle_glue.h`, `firmware/src/ui/idle_glue.cpp`
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Header**

`firmware/src/ui/idle_glue.h`:

```cpp
#pragma once
#include <stdbool.h>

// Bridges core/idle to the panel. Reads dim/sleep timeouts from NVS, watches LVGL's inactivity
// clock, and applies brightness (active -> dim -> off) on phase change. Touch resets LVGL
// inactivity for free (wake-on-touch); IMU wake calls lv_disp_trigger_activity (Task 10).
#ifdef __cplusplus
extern "C" {
#endif

void idle_init(void);                  // call once after lvgl_port_begin()
void idle_service(void);               // call every loop() iteration
void idle_apply_config_from_nvs(void); // re-read timeouts after a settings change
bool idle_is_asleep(void);             // true while the panel is blanked (for wake-touch swallow)

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implementation**

`firmware/src/ui/idle_glue.cpp`:

```cpp
#include "ui/idle_glue.h"
#include "core/idle.h"
#include "core/nvs.h"
#include "ui/durations.h"
#include "hal/display.h"
#include <lvgl.h>

#define IDLE_DIM_RAW 24   // ~9% backlight while dimmed; on AMOLED this is clearly "asleep soon"

static uint32_t     s_dim_ms   = 0;
static uint32_t     s_sleep_ms = 0;
static idle_phase_t s_phase    = IDLE_ACTIVE;

static uint8_t clamp_idx(uint8_t i, uint8_t def) { return i < DURATION_COUNT ? i : def; }

void idle_apply_config_from_nvs(void) {
  s_dim_ms   = DURATIONS[clamp_idx(nvs_get_dim_idx(DURATION_DEFAULT_DIM), DURATION_DEFAULT_DIM)].ms;
  s_sleep_ms = DURATIONS[clamp_idx(nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP), DURATION_DEFAULT_SLEEP)].ms;
}

void idle_init(void) {
  idle_apply_config_from_nvs();
  s_phase = IDLE_ACTIVE;
}

bool idle_is_asleep(void) { return s_phase == IDLE_SLEEP; }

void idle_service(void) {
  uint32_t inact = lv_disp_get_inactive_time(NULL);
  idle_phase_t p = idle_eval(inact, s_dim_ms, s_sleep_ms);
  if (p == s_phase) return;
  switch (p) {
    case IDLE_ACTIVE: display_brightness(nvs_get_brightness(204)); break;
    case IDLE_DIM:    display_brightness(IDLE_DIM_RAW);            break;
    case IDLE_SLEEP:  display_brightness(0);                       break;
  }
  s_phase = p;
}
```

Note: on AMOLED, brightness 0 blanks the panel (pixels off ≈ no draw power). True DCS sleep-in is a possible later optimization; not required for acceptance.

- [ ] **Step 3: Wire into main.cpp**

In `firmware/src/main.cpp`, add the include with the other `ui/` includes:

```cpp
#include "ui/idle_glue.h"
```

In `setup()`, after `carousel_init();` (so brightness restore at boot still wins), add:

```cpp
  idle_init();
```

In `loop()`, add `idle_service();` after `lvgl_port_tick();`:

```cpp
  lvgl_port_tick();
  idle_service();
  delay(5);
```

- [ ] **Step 4: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -8`
Expected: succeeds (Tasks 5, 6, 7 now resolve together).

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui/idle_glue.h firmware/src/ui/idle_glue.cpp firmware/src/main.cpp
git commit -m "feat(idle): drive dim/sleep brightness from LVGL inactivity"
```

---

## Task 8: Swallow the wake-from-sleep touch

**Files:**
- Modify: `firmware/src/ui/lvgl_port.cpp`

When asleep, the first tap should only wake the screen, not also activate whatever is under the finger.

- [ ] **Step 1: Gate the press in the indev read callback**

In `firmware/src/ui/lvgl_port.cpp`, add the include near the top:

```cpp
#include "ui/idle_glue.h"
```

Modify `indev_read_cb` (around line 34). Current:

```cpp
static void indev_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  int16_t x, y;
  if (touch_read(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
```

Change to swallow a press that arrives while asleep — report it released, but let it have already reset LVGL inactivity (the read itself counts as activity, so `idle_service()` will wake on the next loop):

```cpp
static void indev_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  int16_t x, y;
  if (touch_read(&x, &y)) {
    if (idle_is_asleep()) {                  // wake only; don't activate what's under the finger
      lv_disp_trig_activity(NULL);           // LVGL counts only PRESSED as activity; force the reset
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
```

(Leave the existing `x`/`y` assignment and the `else` branch unchanged.)

Note: the explicit `lv_disp_trig_activity(NULL)` is **required**, not optional — verified against `firmware/.pio/libdeps/beacon/lvgl/src/core/lv_indev.c`, which updates `last_activity_time` only on `LV_INDEV_STATE_PRESSED`. Without it, the swallowed (RELEASED) wake touch leaves the inactivity timer untouched and the panel re-sleeps immediately. (Function is `lv_disp_trig_activity`, not `..._trigger_activity`, in LVGL 8.4.)

- [ ] **Step 2: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -5`
Expected: succeeds.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui/lvgl_port.cpp
git commit -m "feat(idle): wake-from-sleep tap wakes only, no pass-through"
```

---

## Task 9: Pull-to-refresh — fetch API + swipe-down gesture

**Files:**
- Modify: `firmware/src/core/fetch_task.h`, `firmware/src/core/fetch_task.cpp`
- Modify: `firmware/src/ui/carousel.cpp`

- [ ] **Step 1: Declare the refresh entry point**

In `firmware/src/core/fetch_task.h`, inside the `extern "C"` block:

```cpp
void fetch_task_refresh_all(void);   // mark every source due now (user pull-to-refresh)
```

**Cross-core safety:** the gesture callback runs in the LVGL/`loop()` context (Core-1); `fetch_task` owns `s_next_due[]` and runs its scheduler loop on Core-0. The callback must **not** write `s_next_due[]` directly. Use a one-shot `volatile` request flag the Core-0 loop consumes.

In `firmware/src/core/fetch_task.cpp`, add the flag near the other file-scope state (next to `s_next_due`):

```cpp
static volatile bool s_refresh_req = false;
```

Add the public function (a plain flag set is the only cross-core write — a single aligned bool is atomic on Xtensa):

```cpp
void fetch_task_refresh_all(void) { s_refresh_req = true; }   // Core-1 UI -> Core-0 scheduler
```

Consume it inside the existing Core-0 `for(;;)` loop. Right after `ds_tick_staleness(now);` (and before the `up`/`!up` branch), add:

```cpp
    if (s_refresh_req) {
      s_refresh_req = false;
      for (int i = 0; i < slots; i++) s_next_due[i] = 0;   // arm every source; due on this sweep
    }
```

(`s_next_due`, `slots`, and `now` are already in scope there — confirmed in `firmware/src/core/fetch_task.cpp`.)

- [ ] **Step 3: Add the swipe-down gesture in carousel**

In `firmware/src/ui/carousel.cpp`, add the include:

```cpp
#include "core/fetch_task.h"
```

The carousel scrolls horizontally only (`LV_DIR_HOR`), so a vertical gesture does not collide with scroll. Add a gesture handler that fires refresh when swiping down on a data screen (Home = page 0, Finance = page 1):

```cpp
static void gesture_cb(lv_event_t* e) {
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_BOTTOM) return;
  int scr = carousel_current();
  if (scr == 0 || scr == 1) fetch_task_refresh_all();   // Home weather + Finance tickers
}
```

In `carousel_init()`, after `s_pager` is created (it is a child of `lv_scr_act()` per `carousel.cpp:86`), register the handler. **Must** clear `LV_OBJ_FLAG_GESTURE_BUBBLE` first — LVGL sets it by default on every parented object (`lv_obj.c:445`), which would bubble the gesture up to the screen and never fire on `s_pager`:

```cpp
  lv_obj_clear_flag(s_pager, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(s_pager, gesture_cb, LV_EVENT_GESTURE, NULL);
```

Note: AI Usage (page 2) is intentionally excluded — refreshing it requires a new hub BLE RX command (out of scope, see plan header).

- [ ] **Step 4: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -5`
Expected: succeeds. (`fetch_task` is compiled out in `BEACON_DEV=1`; the default build is `BEACON_DEV=0`, so the symbol exists. If a dev build is ever made, guard the call — but default build is fine.)

- [ ] **Step 5: Commit**

```bash
git add firmware/src/core/fetch_task.h firmware/src/core/fetch_task.cpp firmware/src/ui/carousel.cpp
git commit -m "feat(refresh): swipe-down pull-to-refresh on Home + Finance"
```

---

## Task 10: Pure IMU gesture detector (`core/imu_detect`)

**Files:**
- Create: `firmware/src/core/imu_detect.h`, `firmware/src/core/imu_detect.cpp`
- Test: `firmware/test/test_imu_detect/test_main.cpp`

Keep the math pure and host-tested; the HAL (Task 11) only does I2C and feeds samples.

- [ ] **Step 1: Write the failing test**

`firmware/test/test_imu_detect/test_main.cpp`:

```cpp
#include <unity.h>
#include "core/imu_detect.h"

void setUp(void) { imu_detect_reset(); } void tearDown(void) {}

// Accel in g. Resting flat ~ (0,0,1). A shake = large deviation from 1g magnitude sustained
// briefly. A raise = z drops fast (device tilted up) crossing the raise threshold.
static void feed(float x, float y, float z, uint32_t t_ms) {
  imu_detect_feed(x, y, z, t_ms);
}

static void test_resting_no_event(void) {
  uint32_t t = 0;
  for (int i = 0; i < 10; i++, t += 50) feed(0.0f, 0.0f, 1.0f, t);
  TEST_ASSERT_EQUAL_INT(IMU_NONE, imu_detect_poll());
}

static void test_shake_fires(void) {
  uint32_t t = 0;
  // alternating hard jerks well above 1g magnitude
  for (int i = 0; i < 6; i++, t += 40) feed(i % 2 ? 2.5f : -2.5f, 0.0f, 1.0f, t);
  TEST_ASSERT_EQUAL_INT(IMU_SHAKE, imu_detect_poll() & IMU_SHAKE ? IMU_SHAKE : IMU_NONE);
}

static void test_raise_fires(void) {
  uint32_t t = 0;
  feed(0.0f, 0.0f, 1.0f, t); t += 60;     // flat
  feed(0.0f, 0.7f, 0.2f, t);              // tilted up quickly => z dropped past threshold
  TEST_ASSERT_EQUAL_INT(IMU_RAISE, imu_detect_poll() & IMU_RAISE ? IMU_RAISE : IMU_NONE);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_resting_no_event);
  RUN_TEST(test_shake_fires);
  RUN_TEST(test_raise_fires);
  return UNITY_END();
}
```

- [ ] **Step 2: Header**

`firmware/src/core/imu_detect.h`:

```cpp
#pragma once
#include <stdint.h>

// Pure IMU gesture detection (FR-PLAT-6). Fed accelerometer samples (g, plus a ms timestamp);
// emits a bitmask of recognized gestures. No I2C/Arduino — host-tested. Thresholds are starting
// values; tune on hardware (Task 11 acceptance).
#ifdef __cplusplus
extern "C" {
#endif

enum { IMU_NONE = 0, IMU_RAISE = 1 << 0, IMU_SHAKE = 1 << 1 };

void    imu_detect_reset(void);
void    imu_detect_feed(float ax, float ay, float az, uint32_t t_ms);
uint8_t imu_detect_poll(void);   // returns + clears pending events since last poll

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Implement**

`firmware/src/core/imu_detect.cpp`:

```cpp
#include "core/imu_detect.h"
#include <math.h>

// Tunables (refine on hardware):
#define SHAKE_G        1.8f    // |a| deviation from 1g that counts as a jolt
#define SHAKE_HITS     3       // jolts within the window => shake
#define SHAKE_WIN_MS   400
#define RAISE_DZ       0.5f    // z drop between consecutive samples => raise (tilt up)
#define RAISE_MAX_DT   200     // only if samples are close in time (a deliberate motion)

static float    s_lastz;
static uint32_t s_lastt;
static int      s_hits;
static uint32_t s_first_hit;
static uint8_t  s_pending;
static bool     s_have_last;

void imu_detect_reset(void) {
  s_lastz = 1.0f; s_lastt = 0; s_hits = 0; s_first_hit = 0; s_pending = 0; s_have_last = false;
}

void imu_detect_feed(float ax, float ay, float az, uint32_t t_ms) {
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  if (fabsf(mag - 1.0f) >= SHAKE_G) {
    if (s_hits == 0 || (t_ms - s_first_hit) > SHAKE_WIN_MS) { s_hits = 1; s_first_hit = t_ms; }
    else s_hits++;
    if (s_hits >= SHAKE_HITS) { s_pending |= IMU_SHAKE; s_hits = 0; }
  }
  if (s_have_last && (t_ms - s_lastt) <= RAISE_MAX_DT && (s_lastz - az) >= RAISE_DZ) {
    s_pending |= IMU_RAISE;
  }
  s_lastz = az; s_lastt = t_ms; s_have_last = true;
}

uint8_t imu_detect_poll(void) {
  uint8_t e = s_pending; s_pending = 0; return e;
}
```

- [ ] **Step 4: Register TU + run test**

Append `+<core/imu_detect.cpp>` to `[env:native]` `build_src_filter` in `platformio.ini`.

Run: `cd firmware && pio test -e native -f test_imu_detect`
Expected: PASS (3 tests). If `test_raise_fires` or `test_shake_fires` fail, adjust the test inputs to clear the thresholds (the detector logic is the contract, not the exact stimulus).

- [ ] **Step 5: Commit**

```bash
git add firmware/src/core/imu_detect.h firmware/src/core/imu_detect.cpp firmware/test/test_imu_detect/test_main.cpp firmware/platformio.ini
git commit -m "feat(imu): pure gesture detector (raise/shake) + host tests"
```

---

## Task 11: QMI8658 HAL bring-up

**Files:**
- Create: `firmware/src/hal/imu.h`, `firmware/src/hal/imu.cpp`

The Waveshare ESP32-S3-Touch-AMOLED-2.16 carries a QMI8658 6-axis IMU on the shared I2C bus (same `Wire` as touch/RTC/PMU). SensorLib 0.3.3 provides `SensorQMI8658`. I2C is serviced on Core-1 (per `main.cpp` comments — touch/RTC are serialized there), so poll the IMU from `loop()`, not a Core-0 task.

- [ ] **Step 1: Header**

`firmware/src/hal/imu.h`:

```cpp
#pragma once
#include <stdbool.h>

// QMI8658 6-axis IMU over the shared Wire bus. imu_begin() assumes power_begin() ran (Wire up).
// imu_poll() reads the accelerometer, feeds core/imu_detect, and returns its event bitmask
// (IMU_RAISE / IMU_SHAKE / IMU_NONE). Call from loop() (Core-1, serialized with touch/RTC on I2C).
#ifdef __cplusplus
extern "C" {
#endif

bool    imu_begin(void);   // true if the QMI8658 answered on I2C
uint8_t imu_poll(void);    // sample + detect; IMU_NONE if no device or no gesture

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implementation**

`firmware/src/hal/imu.cpp` (verify the I2C address and `Wire` pins against `hal/touch.cpp` / `config/pins.h` — reuse the same bus instance and SDA/SCL the other sensors use):

```cpp
#include "hal/imu.h"
#include "core/imu_detect.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

static SensorQMI8658 s_qmi;
static bool          s_ok = false;
static uint32_t      s_last_ms = 0;

#define IMU_POLL_MS 40   // ~25 Hz; enough for raise/shake, light on the shared bus

bool imu_begin(void) {
  // QMI8658 default I2C address is 0x6B (QMI8658_L_SLAVE_ADDRESS). Reuse the existing Wire
  // (already begun by power/touch); do NOT call Wire.begin() again with different pins.
  s_ok = s_qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  if (!s_ok) return false;
  s_qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_250Hz);
  s_qmi.enableAccelerometer();
  imu_detect_reset();
  return true;
}

uint8_t imu_poll(void) {
  if (!s_ok) return IMU_NONE;
  uint32_t now = millis();
  if (now - s_last_ms < IMU_POLL_MS) return IMU_NONE;
  s_last_ms = now;
  if (!s_qmi.getDataReady()) return IMU_NONE;
  IMUdata acc;
  if (!s_qmi.getAccelerometer(acc.x, acc.y, acc.z)) return IMU_NONE;
  imu_detect_feed(acc.x, acc.y, acc.z, now);   // SensorLib reports accel in g
  return imu_detect_poll();
}
```

API verified against installed `lewisxhe/SensorLib@0.3.3` at `firmware/.pio/libdeps/beacon/SensorLib/src/SensorQMI8658.hpp` and `REG/QMI8658Constants.h`: class `SensorQMI8658`, header `SensorQMI8658.hpp`, methods `begin/configAccelerometer/enableAccelerometer/getAccelerometer/getDataReady`, address macro `QMI8658_L_SLAVE_ADDRESS` (== 0x6B), and `getAccelerometer(float&,float&,float&)` returns scaled g values — all confirmed present.

- [ ] **Step 3: Build (device) and flash to confirm the chip answers**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -8`
Expected: succeeds. Then on hardware add a temporary `LOGI("imu begin=%d", imu_begin())` in `setup()` and confirm `imu begin=1` over serial. Remove the temp log after.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/hal/imu.h firmware/src/hal/imu.cpp
git commit -m "feat(imu): QMI8658 HAL bring-up over shared I2C"
```

---

## Task 12: Overlay dismiss dispatcher + expose closes

**Files:**
- Modify: `firmware/src/ui/theme_panel.h`, `firmware/src/ui/theme_panel.cpp`, `firmware/src/ui/wifi_panel.h`, `firmware/src/ui/wifi_panel.cpp`
- Create: `firmware/src/ui/overlays.h`, `firmware/src/ui/overlays.cpp`

`duration_panel` already exposes `duration_panel_close()` (Task 4). Give `theme_panel` and `wifi_panel` the same, then a single dispatcher for shake.

- [ ] **Step 1: Expose theme_panel close**

In `firmware/src/ui/theme_panel.h`, add to the `extern "C"` block:

```cpp
void theme_panel_close(void);
```

In `firmware/src/ui/theme_panel.cpp`, the existing `static void close_panel(void)` does the work. Add a public wrapper at the bottom:

```cpp
void theme_panel_close(void) { close_panel(); }
```

- [ ] **Step 2: Expose wifi_panel close**

In `firmware/src/ui/wifi_panel.h`, add to its `extern "C"` block:

```cpp
void wifi_panel_close(void);
```

In `firmware/src/ui/wifi_panel.cpp`, it has a `static void close_panel(void)`. Add:

```cpp
void wifi_panel_close(void) { close_panel(); }
```

(If `wifi_panel` has open sub-modals like the add/confirm cards, `close_panel` already tears the root down — verify it fully dismisses.)

- [ ] **Step 3: Dispatcher**

`firmware/src/ui/overlays.h`:

```cpp
#pragma once
#include <stdbool.h>

// Closes the topmost open modal overlay, if any. Used by the IMU shake gesture (FR-PLAT-6).
// Returns true if something was dismissed.
#ifdef __cplusplus
extern "C" {
#endif
bool ui_dismiss_top_overlay(void);
#ifdef __cplusplus
}
#endif
```

`firmware/src/ui/overlays.cpp`:

```cpp
#include "ui/overlays.h"
#include "ui/theme_panel.h"
#include "ui/wifi_panel.h"
#include "ui/duration_panel.h"

bool ui_dismiss_top_overlay(void) {
  if (duration_panel_is_open()) { duration_panel_close(); return true; }
  if (theme_panel_is_open())    { theme_panel_close();    return true; }
  if (wifi_panel_is_open())     { wifi_panel_close();     return true; }
  return false;
}
```

- [ ] **Step 4: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -5`
Expected: succeeds.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/ui/theme_panel.h firmware/src/ui/theme_panel.cpp firmware/src/ui/wifi_panel.h firmware/src/ui/wifi_panel.cpp firmware/src/ui/overlays.h firmware/src/ui/overlays.cpp
git commit -m "feat(ui): overlay dismiss dispatcher for shake gesture"
```

---

## Task 13: Wire IMU gestures into the loop

**Files:**
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Includes + init**

In `firmware/src/main.cpp`, add includes:

```cpp
#include "hal/imu.h"
#include "core/imu_detect.h"
#include "ui/overlays.h"
```

In `setup()`, after `touch_begin();` (shared Wire is up), add:

```cpp
  if (!imu_begin()) LOGE("imu: not detected (gestures disabled)");
```

- [ ] **Step 2: Poll + act in loop()**

In `loop()`, after `idle_service();`, add:

```cpp
  uint8_t g = imu_poll();
  if (g & IMU_RAISE) lv_disp_trig_activity(NULL);   // wake from dim/sleep
  if (g & IMU_SHAKE) ui_dismiss_top_overlay();
```

(`lv_disp_trig_activity(NULL)` — LVGL 8.4 spelling, not `..._trigger_activity` — resets LVGL inactivity, so the next `idle_service()` returns ACTIVE and restores brightness, waking without a synthetic touch.)

- [ ] **Step 3: Build**

Run: `cd firmware && pio run -e beacon 2>&1 | tail -5`
Expected: succeeds.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "feat(imu): raise wakes display, shake dismisses overlay"
```

---

## Task 14: On-device acceptance pass

**Files:** none (hardware verification + threshold tuning).

- [ ] **Step 1: Flash**

Run: `cd firmware && pio run -e beacon -t upload && pio device monitor`

- [ ] **Step 2: Idle dim/sleep (all screens)**

For at least Home, Finance, AI Usage, Settings: leave idle; confirm the panel dims at the `Dim after` timeout and blanks at `Sleep after`. Confirm a tap wakes it and the tap does **not** also trigger a row/action (Task 8).

- [ ] **Step 3: Never option (#4)**

Set `Dim after = Never`: confirm it never dims. Set `Sleep after = Never`: confirm it never blanks. Set both to short values and confirm dim precedes sleep.

- [ ] **Step 4: Settings picker UX (#5)**

Confirm `Dim` and `Sleep` rows each open a list of presets with the current one marked (same feel as Theme), selecting one updates the row label and takes effect immediately, and the choice survives a reboot.

- [ ] **Step 5: Swipe-down refresh (#3)**

On Home and Finance, swipe down; confirm an immediate re-fetch (watch serial fetch logs / screen state flips to loading then live). Confirm AI Usage swipe-down does nothing (expected — out of scope).

- [ ] **Step 6: IMU gestures**

Raise/flick the device while dimmed/asleep → wakes. Open the Theme or a duration picker, then shake → it closes. Tune `core/imu_detect.cpp` thresholds (`SHAKE_G`, `SHAKE_HITS`, `RAISE_DZ`, timing) if normal desk handling false-triggers or real gestures miss; re-run `pio test -e native -f test_imu_detect` after edits.

- [ ] **Step 7: Regression**

Confirm horizontal swipe still cycles all screens both directions, brightness setting still works, BLE/WiFi planes still run (AI Usage live, weather/finance live).

- [ ] **Step 8: Commit any threshold tuning**

```bash
git add firmware/src/core/imu_detect.cpp
git commit -m "tune(imu): hardware-tuned gesture thresholds"
```

---

## Self-review notes

- **Spec coverage:** FR-PLAT-7 dim/sleep/wake-on-touch (Tasks 1,7,8); configurable timers + Never (Tasks 2,3,4,5,6); FR-PLAT-6 IMU raise=wake/shake=dismiss (Tasks 10,11,12,13); swipe-down refresh (Task 9). FR-PLAT-5 long-press intentionally dropped (documented). AI Usage refresh intentionally out of scope (documented).
- **Type consistency:** `idle_phase_t` {IDLE_ACTIVE/DIM/SLEEP} used identically across idle.h, idle_glue.cpp. `idle_apply_config_from_nvs` defined in idle_glue.h, called from settings_power_rows.cpp. `duration_pick_cb` signature matches `on_dim`/`on_sleep`. IMU event bitmask {IMU_NONE/RAISE/SHAKE} shared by imu_detect.h, imu.cpp, main.cpp.
- **Cross-task dependency:** Tasks 5 and 6 reference `idle_glue.h` (Task 7). Execute Task 7 before the first full device build, or accept that Tasks 5/6 only compile-check their own headers until 7 lands. Subagent-driven execution should order 1→2→3→4→7→5→6→8→9→10→11→12→13→14 if strict build-green-per-task is required.

## Codex review — applied corrections

This plan was reviewed against the installed libs/codebase. Fixes folded in:

- LVGL 8.4 activity reset is `lv_disp_trig_activity()` (not `..._trigger_activity`) — Tasks 8, 13.
- Swallowed wake touch **must** call `lv_disp_trig_activity(NULL)` — LVGL counts only PRESSED as activity (Task 8, now required not optional).
- Swipe-down gesture: clear `LV_OBJ_FLAG_GESTURE_BUBBLE` on `s_pager` or the gesture never reaches it (Task 9).
- `fetch_task_refresh_all()` is cross-core (Core-1 UI → Core-0 scheduler): use a `volatile` request flag, not a direct `s_next_due[]` write (Task 9).
- QMI8658 address macro is `QMI8658_L_SLAVE_ADDRESS` (no trailing `_L`); API names verified (Task 11).
- `idle_apply_config_from_nvs()` clamps the persisted index against `DURATION_COUNT` (Task 7).
- libdeps live under `firmware/.pio/libdeps/beacon/...`.

Confirmed OK by review: QMI8658 is the board's IMU on shared I2C (SDA15/SCL14); `s_next_due[]` / `ds_get_finance_count()` in scope; horizontal scroll does not consume the vertical gesture.

## Open items to confirm during execution

1. **Per-view row layout** — the 6 non-editorial settings views have bespoke y-rhythm; Task 6 Step 5 adapts per file.
2. **IMU thresholds** — `core/imu_detect.cpp` constants are starting values; tune on hardware (Task 14 Step 6).
