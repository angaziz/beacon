#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "config/layout.h"
#include "hal/display.h"
#include "hal/power.h"
#include "core/net.h"
#include "core/nvs.h"
#include "ui/screens/screen_common.h"
#include "ui/wifi_panel.h"
#include "ui/theme_panel.h"
#include "ui/about_panel.h"
#include "ui/settings_power_rows.h"
#include <Arduino.h>

// Aerospace HUD / Settings. "// SETTINGS" eyebrow + version (right) + a list of rows:
// Wi-Fi, Brightness, Theme, Sleep, About. INTERACTIVE: Theme tap opens the theme
// picker (theme_panel), Brightness tap cycles 40/60/80/100% (display_brightness inline).
// Other rows are static.

static lv_obj_t *s_theme_val;
static lv_obj_t *s_bright_val;
static lv_obj_t *s_batt_val;
static lv_obj_t *s_wifi_val;
static lv_obj_t *s_dim_val;
static lv_obj_t *s_sleep_val;

static const uint8_t BRIGHT_STEPS[] = { 40, 60, 80, 100 };
static uint8_t s_bright_idx = 2;   // 80%

static void theme_tap(lv_event_t* e) { (void)e; theme_panel_open(); }
static void about_cb(lv_event_t*) { about_panel_open(); }

static void wifi_open_cb(lv_event_t*) { wifi_panel_open(); }
static void dim_cb(lv_event_t*)   { settings_power_open_dim(); }
static void sleep_cb(lv_event_t*) { settings_power_open_sleep(); }

static void bright_tap(lv_event_t* e) {
  (void)e;
  s_bright_idx = (uint8_t)((s_bright_idx + 1) % (sizeof(BRIGHT_STEPS) / sizeof(BRIGHT_STEPS[0])));
  uint8_t pct = BRIGHT_STEPS[s_bright_idx];
  display_brightness((uint8_t)(pct * 255 / 100));
  nvs_set_brightness((uint8_t)(pct * 255 / 100));
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
  lv_label_set_text(s_bright_val, buf);
}

// One row: bold-ish label (left, display font) + value (right, mono dim) + hairline rule.
// Returns the value label so callers can store/update it. interactive => clickable + cb.
static lv_obj_t* make_row(lv_obj_t* list, const beacon_theme_t* t, const char* name,
                          const char* value, lv_event_cb_t cb) {
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, 50);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_border_color(row, t->line, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);

  lv_obj_t* lbl = lv_label_create(row);
  lv_obj_set_style_text_font(lbl, t->f_display, 0);
  lv_obj_set_style_text_color(lbl, t->ink, 0);
  lv_label_set_text(lbl, name);

  lv_obj_t* val = lv_label_create(row);
  lv_obj_set_style_text_font(val, t->f_mono, 0);
  lv_obj_set_style_text_color(val, cb ? t->accent : t->ink_dim, 0);
  lv_label_set_text(val, value);

  if (cb) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
  }
  return val;
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* title = lv_label_create(page);
  lv_obj_add_style(title, &S.slot, 0);
  lv_label_set_text(title, "// SETTINGS");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  lv_obj_t* ver = lv_label_create(page);
  lv_obj_add_style(ver, &S.slot, 0);
  lv_label_set_text(ver, "V0.1");
  lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* list = lv_obj_create(page);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 36);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 30);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(list, 0, 0);

  s_wifi_val     = make_row(list, t, "Wi-Fi", "not set >", NULL);
  lv_obj_t* wrow = lv_obj_get_parent(s_wifi_val); lv_obj_add_flag(wrow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wrow, wifi_open_cb, LV_EVENT_CLICKED, NULL);
  s_batt_val     = make_row(list, t, "Battery", "--", NULL);
  s_bright_idx = bright_step_for_nvs(BRIGHT_STEPS, sizeof(BRIGHT_STEPS) / sizeof(BRIGHT_STEPS[0]));
  char bb[8]; snprintf(bb, sizeof(bb), "%u%%", (unsigned)BRIGHT_STEPS[s_bright_idx]);
  s_bright_val   = make_row(list, t, "Brightness", bb, bright_tap);
  char thv[20]; snprintf(thv, sizeof(thv), "%s >", THEME_CATALOG[theme_index()].id);
  s_theme_val    = make_row(list, t, "Theme", thv, theme_tap);
  s_dim_val      = make_row(list, t, "Dim", "", dim_cb);
  s_sleep_val    = make_row(list, t, "Sleep", "", sleep_cb);
  make_row(list, t, "About", ">", about_cb);
}

static void update(void) {
  char wbuf[48]; net_status_str(wbuf, sizeof(wbuf)); lv_label_set_text_fmt(s_wifi_val, "%s >", wbuf);
  lv_label_set_text_fmt(s_theme_val, "%s >", THEME_CATALOG[theme_index()].id);

  char db[12], sb[12];
  settings_power_dim_label(db, sizeof(db));   lv_label_set_text(s_dim_val, db);
  settings_power_sleep_label(sb, sizeof(sb)); lv_label_set_text(s_sleep_val, sb);

  int pct = power_battery_pct();
  char bt[8];
  if (pct >= 0) snprintf(bt, sizeof(bt), "%d%%%s", pct, power_charging() ? "+" : "");
  else          snprintf(bt, sizeof(bt), "%s", power_charging() ? "USB" : "--");
  lv_label_set_text(s_batt_val, bt);
    lv_obj_set_style_text_color(s_batt_val, power_charging() ? theme_active()->accent : (pct >= 0 && pct <= 20 ? theme_active()->down : theme_active()->ink), 0);
}

extern const screen_view_t settings_hud_view = { build, update };
