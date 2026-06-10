#include "ui/screen.h"
#include "ui/styles.h"
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

// Analog Neo settings: minimal ice-blue rows (label left display, value right mono, hairline
// rule). Theme row taps open the theme picker (theme_panel). Brightness row taps cycle
// 40/60/80/100% (display_brightness inline).

static lv_obj_t *s_theme_val, *s_bright_val, *s_batt_val, *s_wifi_val;
static lv_obj_t *s_dim_val, *s_sleep_val;

static const uint8_t BRIGHT_PCT[] = { 40, 60, 80, 100 };
static uint8_t s_bright_idx = 2;   // 80%

static void theme_tap_cb(lv_event_t*) { theme_panel_open(); }
static void about_cb(lv_event_t*) { about_panel_open(); }

static void wifi_open_cb(lv_event_t*) { wifi_panel_open(); }
static void dim_cb(lv_event_t*)   { settings_power_open_dim(); }
static void sleep_cb(lv_event_t*) { settings_power_open_sleep(); }

static void bright_tap_cb(lv_event_t*) {
  s_bright_idx = (uint8_t)((s_bright_idx + 1) % (sizeof(BRIGHT_PCT) / sizeof(BRIGHT_PCT[0])));
  uint8_t pct = BRIGHT_PCT[s_bright_idx];
  display_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  nvs_set_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  char b[8]; snprintf(b, sizeof(b), "%u%%", pct);
  lv_label_set_text(s_bright_val, b);
}

static lv_obj_t* make_row(lv_obj_t* parent, const beacon_theme_t* t, int y,
                          const char* name, const char* val, bool accent_val,
                          lv_event_cb_t tap) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, SCREEN_W - 2 * SAFE_INSET, 40);
  lv_obj_set_pos(row, SAFE_INSET, y);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  if (tap) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, tap, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, name);
  lv_obj_set_style_text_color(lbl, t->ink, 0);
  lv_obj_set_style_text_font(lbl, t->f_display, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* v = lv_label_create(row);
  lv_label_set_text(v, val);
  lv_obj_set_style_text_color(v, accent_val ? t->accent : t->ink_dim, 0);
  lv_obj_set_style_text_font(v, t->f_mono, 0);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t* rule = lv_obj_create(row);
  lv_obj_remove_style_all(rule);
  lv_obj_set_size(rule, lv_pct(100), t->stroke_hair);
  lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(rule, t->line, 0);
  lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
  return v;
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "settings");
  lv_obj_set_style_text_color(eyebrow, t->ink_dim, 0);
  lv_obj_set_style_text_font(eyebrow, t->f_mono, 0);
  lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  lv_obj_t* ver = lv_label_create(page);
  lv_label_set_text(ver, "v0.1");
  lv_obj_set_style_text_color(ver, t->ink_dim, 0);
  lv_obj_set_style_text_font(ver, t->f_mono, 0);
  lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  const int top = SAFE_INSET + 36;
  const int pitch = 42;   // 7 rows must clear the bottom arc on the 466px round panel
  s_bright_idx = bright_step_for_nvs(BRIGHT_PCT, sizeof(BRIGHT_PCT) / sizeof(BRIGHT_PCT[0]));
  char bb[8]; snprintf(bb, sizeof(bb), "%u%%", BRIGHT_PCT[s_bright_idx]);

  s_wifi_val    = make_row(page, t, top + 0 * pitch, "wi-fi", "not set >", false, NULL);
  lv_obj_t* wrow = lv_obj_get_parent(s_wifi_val); lv_obj_add_flag(wrow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wrow, wifi_open_cb, LV_EVENT_CLICKED, NULL);
  s_batt_val    = make_row(page, t, top + 1 * pitch, "battery", "--", false, NULL);
  s_bright_val  = make_row(page, t, top + 2 * pitch, "brightness", bb, false, bright_tap_cb);
  char thv[20]; snprintf(thv, sizeof(thv), "%s >", THEME_CATALOG[theme_index()].id);
  s_theme_val   = make_row(page, t, top + 3 * pitch, "theme", thv, true, theme_tap_cb);
  s_dim_val     = make_row(page, t, top + 4 * pitch, "dim", "", false, dim_cb);
  s_sleep_val   = make_row(page, t, top + 5 * pitch, "sleep", "", false, sleep_cb);
  make_row(page, t, top + 6 * pitch, "about", ">", false, about_cb);
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

extern const screen_view_t settings_analog_view = { build, update };
