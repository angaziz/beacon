#include "ui/carousel.h"
#include "ui/carousel_nav.h"
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/chrome.h"
#include "core/nvs.h"
#include "core/net.h"   // net_is_enabled: drives which screens are visible
#include "config/layout.h"
#include "ui/screens/screen_home.h"
#include "ui/screens/screen_finance.h"
#include "ui/screens/screen_usage.h"
#include "ui/screens/screen_buddy.h"
#include "ui/screens/screen_settings.h"

// Every screen that can exist. The carousel shows a runtime-filtered subset of these (see
// rebuild_visible): with Wi-Fi switched off, Finance has no reachable data source and is dropped
// rather than left showing OFFLINE forever. Home stays -- its clock is RTC-backed and its weather
// arrives from the hub over BLE (CONTRACT.md §A).
static const screen_module_t* ALL_MODULES[] = {
  &home_module, &finance_module, &usage_module, &buddy_module, &settings_module,
};
static const int ALL_COUNT = (int)(sizeof(ALL_MODULES) / sizeof(ALL_MODULES[0]));

// s_visible[slot] = index into ALL_MODULES. COUNT is now runtime, so every place that used to read
// the compile-time count goes through s_count. Indices persisted to NVS / used by carousel_goto_buddy
// are LOGICAL (ALL_MODULES) indices, so they survive the visible set changing under them.
static int s_visible[8];
static int s_count = 0;

static lv_obj_t* s_pager = nullptr;
static lv_obj_t* s_dotbar = nullptr;
static lv_obj_t* s_pages[8];
static lv_obj_t* s_dots[8];
static int s_current = 0;   // slot index into s_visible, NOT a logical index

static const screen_module_t* mod(int slot) { return ALL_MODULES[s_visible[slot]]; }
static int logical_of(int slot) { return s_visible[slot]; }

// Slot currently showing logical screen `lg`, or -1 when it is filtered out.
static int slot_of_logical(int lg) {
  for (int i = 0; i < s_count; i++) if (s_visible[i] == lg) return i;
  return -1;
}
static bool s_settling = false;   // guards reentrant SCROLL_END from our own recenter()
static lv_timer_t* s_tick = nullptr;   // the 500ms visible-screen update timer; paused while idle (#60)

static void set_dots(int active) {
  const beacon_theme_t* t = theme_active();
  for (int i = 0; i < s_count; i++)
    lv_obj_set_style_bg_color(s_dots[i], i == active ? t->accent : t->line, 0);
}

static void show(int idx) {
  s_current = idx;
  set_dots(idx);
  if (mod(idx)->update) mod(idx)->update();
  // Persist the LOGICAL index: a slot index would point at a different screen (or none) after the
  // visible set changes, so a reboot with Wi-Fi off could restore a screen that no longer exists.
  nvs_set_screen((uint8_t)logical_of(idx));   // persist last screen (FR-PLAT-3)
}

// Theme hook: per-theme LAYOUTS differ, so a theme switch rebuilds every page (clear + chrome +
// the new theme's view), not just a restyle. Cheap enough with the LVGL pool in PSRAM.
static void on_theme(const beacon_theme_t* t) {
  styles_rebuild(t);
  for (int i = 0; i < s_count; i++) {
    lv_obj_clean(s_pages[i]);
    chrome_attach(s_pages[i]);
    mod(i)->build(s_pages[i]);
    // Populate every page now so a freshly-built label never shows LVGL's default "Text"
    // when it scrolls into view before its first tick. Off-screen invalidations are clipped.
    if (mod(i)->update) mod(i)->update();
  }
  set_dots(s_current);
}

// Reorder the page objects so s_current sits at the center slot with its circular neighbours
// on both sides, then pin the scroll to that slot without animation. The page under the
// viewport is unchanged pixels, so the rearrange is invisible -- it just guarantees a real
// neighbour exists in both directions for the next swipe, making the wrap boundary an ordinary
// one-page move (FR fix #5). LV_OBJ_FLAG_SCROLL_ONE caps a gesture at one page, so neighbours
// on each side are always sufficient.
static void recenter(void) {
  s_settling = true;   // scroll_to_x(LV_ANIM_OFF) re-emits SCROLL_END synchronously
  for (int slot = 0; slot < s_count; slot++)
    lv_obj_move_to_index(s_pages[carousel_logical_at(s_current, slot, s_count)], slot);
  lv_obj_update_layout(s_pager);
  lv_obj_scroll_to_x(s_pager, carousel_center_slot(s_count) * SCREEN_W, LV_ANIM_OFF);
  s_settling = false;
}

static void scrollend_cb(lv_event_t*) {
  if (s_settling) return;
  int slot = carousel_index_for_x(lv_obj_get_scroll_x(s_pager), SCREEN_W, s_count);
  if (slot == carousel_center_slot(s_count)) return;   // bounced back to center: no page change
  show(carousel_logical_at(s_current, slot, s_count));
  recenter();                                          // re-pin so the next swipe has both neighbours
}

// Fill s_visible/s_count from the current Wi-Fi state. Returns true if the set changed.
static bool compute_visible(void) {
  int prev[8], prev_n = s_count;
  for (int i = 0; i < s_count; i++) prev[i] = s_visible[i];

  // "Usable" means both: the radio is not switched off AND at least one network is saved. Forgetting
  // the last network leaves net_is_enabled() true, so gating on the toggle alone would keep Finance
  // on screen with nothing to fetch -- which is exactly what it must not do.
  const bool wifi = net_is_enabled() && nvs_has_wifi();
  s_count = 0;
  for (int i = 0; i < ALL_COUNT; i++) {
    // Finance is the only screen with no data path at all without Wi-Fi (Yahoo/Binance over HTTPS).
    if (!wifi && ALL_MODULES[i] == &finance_module) continue;
    s_visible[s_count++] = i;
  }

  if (prev_n != s_count) return true;
  for (int i = 0; i < s_count; i++) if (prev[i] != s_visible[i]) return true;
  return false;
}

// (Re)create one page + one dot per visible slot. Safe to call again: both parents are cleaned first.
static void build_slots(void) {
  lv_obj_clean(s_pager);
  lv_obj_clean(s_dotbar);
  for (int i = 0; i < s_count; i++) {
    lv_obj_t* page = lv_obj_create(s_pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, SCREEN_W, SCREEN_H);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(page, 0, 0);
    s_pages[i] = page;

    lv_obj_t* d = lv_obj_create(s_dotbar);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 6, 6);
    lv_obj_set_style_radius(d, 3, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    s_dots[i] = d;
  }
}

// Re-filter after a Wi-Fi toggle. Keeps the user on the same LOGICAL screen where possible; if the
// screen they were on just disappeared, lands on Home rather than whatever slid into that slot.
static void rebuild_visible(void) {
  const int was = logical_of(s_current);
  if (!compute_visible()) return;

  build_slots();
  on_theme(theme_active());   // rebuild every page's content into the new slot objects

  int slot = slot_of_logical(was);
  if (slot < 0) slot = 0;     // was on the screen that just went away => Home
  s_current = slot;
  recenter();
  show(slot);
}

static void tick_cb(lv_timer_t*) {
  rebuild_visible();   // cheap no-op unless the Wi-Fi toggle changed the visible set
  if (mod(s_current)->update) mod(s_current)->update();
  set_dots(s_current);
}

void carousel_init(void) {
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

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
  lv_obj_add_event_cb(s_pager, scrollend_cb, LV_EVENT_SCROLL_END, NULL);

  // Dot indicator on the top layer (does not scroll with pages), bottom arc-free band.
  s_dotbar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_dotbar);
  lv_obj_set_flex_flow(s_dotbar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(s_dotbar, 8, 0);
  lv_obj_set_size(s_dotbar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(s_dotbar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET - 22));
  lv_obj_clear_flag(s_dotbar, LV_OBJ_FLAG_CLICKABLE);

  compute_visible();
  build_slots();   // pages + dots for the current visible set; content built by on_theme() below

  theme_on_apply(on_theme);
  // One-time: apply the compiled default theme when it changes (THEME_DEFAULT_VER bump), without
  // stomping a later manual choice (that updates the theme but leaves the version satisfied).
  if (nvs_get_byte("thmver", 0) < THEME_DEFAULT_VER) {
    nvs_set_theme(DEFAULT_THEME_INDEX);
    nvs_set_byte("thmver", THEME_DEFAULT_VER);
  }
  uint8_t theme0 = nvs_get_theme(DEFAULT_THEME_INDEX); if (theme0 >= THEME_COUNT) theme0 = DEFAULT_THEME_INDEX;
  theme_set(theme0);        // restore persisted theme; builds all pages via on_theme

  // Restore the last screen. The stored value is a LOGICAL index, so map it to a slot; a screen that
  // is filtered out right now (or an out-of-range value from an older build) falls back to Home.
  int start_lg = nvs_get_screen(0);
  int start = (start_lg >= 0 && start_lg < ALL_COUNT) ? slot_of_logical(start_lg) : -1;
  if (start < 0) start = 0;
  s_current = start;
  recenter();                                                      // pin start to the center slot
  show(start);
  s_tick = lv_timer_create(tick_cb, 500, NULL);
}

// #60: pause the per-tick repaint while the panel is dim/asleep so the display can actually sleep
// (no update() => no LVGL invalidations => no QSPI flushes). Resume runs one immediate update() so a
// wake shows current data with no up-to-500ms lag.
void carousel_set_tick_paused(bool paused) {
  if (!s_tick) return;
  if (paused) { lv_timer_pause(s_tick); return; }
  lv_timer_resume(s_tick);
  if (mod(s_current)->update) mod(s_current)->update();
}

int carousel_current(void) { return s_current; }
lv_obj_t* carousel_root(void) { return s_pager; }

// Navigate to the buddy screen. Resolved by module identity, not a fixed index: the visible set is
// filtered at runtime, so buddy's slot shifts when Finance is hidden.
void carousel_goto_buddy(void) {
  int slot = -1;
  for (int i = 0; i < s_count; i++) if (mod(i) == &buddy_module) { slot = i; break; }
  if (slot < 0 || s_current == slot) return;   // not visible, or already there (no scroll churn)
  show(slot);
  recenter();
}

#if BEACON_CAPTURE
int carousel_count(void) { return s_count; }
const char* carousel_screen_id(int idx) { return mod(idx)->id; }
void carousel_goto(int idx) { show(idx); recenter(); }   // same path scrollend_cb uses, sans gesture
#endif

void carousel_set_swipe_enabled(bool en) {
  if (en) { lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR); lv_obj_add_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE); }
  else    { lv_obj_clear_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE); }
}
