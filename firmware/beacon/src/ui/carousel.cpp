#include "ui/carousel.h"
#include "ui/carousel_nav.h"
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/chrome.h"
#include "core/nvs.h"
#include "config/layout.h"
#include "ui/screens/screen_home.h"
#include "ui/screens/screen_finance.h"
#include "ui/screens/screen_usage.h"
#include "ui/screens/screen_buddy.h"
#include "ui/screens/screen_nowplaying.h"
#include "ui/screens/screen_settings.h"

static const screen_module_t* MODULES[] = {
  &home_module, &finance_module, &usage_module, &buddy_module, &nowplaying_module, &settings_module,
};
static const int COUNT = (int)(sizeof(MODULES) / sizeof(MODULES[0]));

static lv_obj_t* s_pager = nullptr;
static lv_obj_t* s_pages[8];
static lv_obj_t* s_dots[8];
static int s_current = 0;
static int s_wrap_target = -1;   // pending circular wrap (executed on scroll settle)

static void set_dots(int active) {
  const beacon_theme_t* t = theme_active();
  for (int i = 0; i < COUNT; i++)
    lv_obj_set_style_bg_color(s_dots[i], i == active ? t->accent : t->line, 0);
}

static void show(int idx) {
  s_current = idx;
  set_dots(idx);
  if (MODULES[idx]->update) MODULES[idx]->update();
  nvs_set_screen((uint8_t)idx);   // persist last screen (FR-PLAT-3)
}

// Theme hook: per-theme LAYOUTS differ, so a theme switch rebuilds every page (clear + chrome +
// the new theme's view), not just a restyle. Cheap enough with the LVGL pool in PSRAM.
static void on_theme(const beacon_theme_t* t) {
  styles_rebuild(t);
  for (int i = 0; i < COUNT; i++) {
    lv_obj_clean(s_pages[i]);
    chrome_attach(s_pages[i]);
    MODULES[i]->build(s_pages[i]);
  }
  set_dots(s_current);
  if (MODULES[s_current]->update) MODULES[s_current]->update();
}

static void scroll_cb(lv_event_t*) {
  int x = lv_obj_get_scroll_x(s_pager);
  int maxx = (COUNT - 1) * SCREEN_W;
  int thr = SCREEN_W / 4;
  if (x > maxx + thr)      s_wrap_target = 0;          // past last => first
  else if (x < -thr)       s_wrap_target = COUNT - 1;  // past first => last
  else                     s_wrap_target = -1;         // back in bounds => cancel pending wrap
}

static void scrollend_cb(lv_event_t*) {
  if (s_wrap_target >= 0) {
    int t = s_wrap_target; s_wrap_target = -1;
    lv_obj_scroll_to_x(s_pager, t * SCREEN_W, LV_ANIM_OFF);   // INSTANT jump (no long multi-page scroll)
    show(t);
    return;
  }
  int idx = carousel_index_for_x(lv_obj_get_scroll_x(s_pager), SCREEN_W, COUNT);
  if (idx != s_current) show(idx);
}

static void tick_cb(lv_timer_t*) {
  if (MODULES[s_current]->update) MODULES[s_current]->update();
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
  lv_obj_add_event_cb(s_pager, scroll_cb, LV_EVENT_SCROLL, NULL);
  lv_obj_add_event_cb(s_pager, scrollend_cb, LV_EVENT_SCROLL_END, NULL);

  for (int i = 0; i < COUNT; i++) {
    lv_obj_t* page = lv_obj_create(s_pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, SCREEN_W, SCREEN_H);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(page, 0, 0);
    s_pages[i] = page;   // content built by on_theme() below
  }

  // Dot indicator on the top layer (does not scroll with pages), bottom arc-free band.
  lv_obj_t* bar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(bar);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(bar, 8, 0);
  lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET - 22));
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < COUNT; i++) {
    lv_obj_t* d = lv_obj_create(bar);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 6, 6);
    lv_obj_set_style_radius(d, 3, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    s_dots[i] = d;
  }

  theme_on_apply(on_theme);
  // One-time: apply the compiled default theme when it changes (THEME_DEFAULT_VER bump), without
  // stomping a later manual choice (that updates the theme but leaves the version satisfied).
  if (nvs_get_byte("thmver", 0) < THEME_DEFAULT_VER) {
    nvs_set_theme(DEFAULT_THEME_INDEX);
    nvs_set_byte("thmver", THEME_DEFAULT_VER);
  }
  uint8_t theme0 = nvs_get_theme(DEFAULT_THEME_INDEX); if (theme0 >= THEME_COUNT) theme0 = DEFAULT_THEME_INDEX;
  theme_set(theme0);        // restore persisted theme; builds all pages via on_theme

  int start = nvs_get_screen(0); if (start >= COUNT) start = 0;   // restore last screen
  if (start != 0) {
    lv_obj_update_layout(s_pager);                                  // positions must exist before scroll
    lv_obj_scroll_to_x(s_pager, start * SCREEN_W, LV_ANIM_OFF);
  }
  show(start);
  lv_timer_create(tick_cb, 500, NULL);
}

int carousel_current(void) { return s_current; }
lv_obj_t* carousel_root(void) { return s_pager; }

void carousel_set_swipe_enabled(bool en) {
  if (en) { lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR); lv_obj_add_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE); }
  else    { lv_obj_clear_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE); }
}
