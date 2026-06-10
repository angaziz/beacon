// Calm Futurism SETTINGS view. Sparse list: big Doto row label on the left, dim value on the
// right, hairline rules between rows. Rows: Wi-Fi, Brightness, Theme, Sleep, About.
// Interactive: Theme tap opens the theme picker (theme_panel); Brightness tap cycles
// 40/60/80/100% (display_brightness inline).
// Background + chrome drawn by the carousel.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/theme_panel.h"
#include "config/layout.h"
#include "hal/display.h"
#include "hal/power.h"
#include "core/net.h"
#include "core/nvs.h"
#include "ui/screens/screen_common.h"
#include "ui/wifi_panel.h"
#include "ui/settings_power_rows.h"
#include <Arduino.h>
static void update(void);

static lv_obj_t *s_theme_val, *s_bright_val, *s_batt_val, *s_wifi_val;
static lv_obj_t *s_dim_val, *s_sleep_val;

static const uint8_t BRIGHT_PCT[] = { 40, 60, 80, 100 };
static uint8_t s_bright_idx = 2;  // default 80%

static void on_theme_tap(lv_event_t* e) { (void)e; theme_panel_open(); }

static void wifi_open_cb(lv_event_t*) { wifi_panel_open(); }
static void dim_cb(lv_event_t*)   { settings_power_open_dim(); }
static void sleep_cb(lv_event_t*) { settings_power_open_sleep(); }

static void on_bright_tap(lv_event_t* e) {
  (void)e;
  s_bright_idx = (s_bright_idx + 1) % (sizeof(BRIGHT_PCT) / sizeof(BRIGHT_PCT[0]));
  uint8_t pct = BRIGHT_PCT[s_bright_idx];
  display_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  nvs_set_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  char b[8];
  snprintf(b, sizeof(b), "%u%%", (unsigned)pct);
  lv_label_set_text(s_bright_val, b);
}

// One settings row: big label left, dim value right, hairline under it. Returns the value label.
static lv_obj_t* mk_row(lv_obj_t* page, const beacon_theme_t* t, int y, const char* name,
                        const char* val, lv_color_t valcol, lv_event_cb_t tap) {
  lv_obj_t* row = lv_obj_create(page);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, SCREEN_W - 2 * SAFE_INSET, 46);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  if (tap) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, tap, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_t* nm = lv_label_create(row);
  lv_label_set_text(nm, name);
  lv_obj_set_style_text_font(nm, t->f_display, 0);
  lv_obj_set_style_text_color(nm, t->ink, 0);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* vl = lv_label_create(row);
  lv_label_set_text(vl, val);
  lv_obj_set_style_text_font(vl, t->f_body, 0);
  lv_obj_set_style_text_color(vl, valcol, 0);
  lv_obj_set_style_text_letter_space(vl, 2, 0);
  lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t* hr = lv_obj_create(row);
  lv_obj_remove_style_all(hr);
  lv_obj_set_size(hr, lv_pct(100), 1);
  lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(hr, t->line, 0);
  lv_obj_align(hr, LV_ALIGN_BOTTOM_MID, 0, 0);

  return vl;
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dot = lv_obj_create(page);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, t->accent, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 4, SAFE_INSET + 12);

  lv_obj_t* brand = lv_label_create(page);
  lv_label_set_text(brand, "settings");
  lv_obj_set_style_text_font(brand, t->f_body, 0);
  lv_obj_set_style_text_color(brand, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(brand, 3, 0);
  lv_obj_align(brand, LV_ALIGN_TOP_LEFT, SAFE_INSET + 20, SAFE_INSET + 8);

  int y = SAFE_INSET + 36;
  const int dy = 48;

  s_wifi_val = mk_row(page, t, y, "Wi-Fi", "not set >", t->ink_dim, NULL); y += dy;
  lv_obj_t* wifi_row = lv_obj_get_parent(s_wifi_val);   // tap the whole row (big touch target)
  lv_obj_add_flag(wifi_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wifi_row, wifi_open_cb, LV_EVENT_CLICKED, NULL);

  s_batt_val = mk_row(page, t, y, "Battery", "--", t->ink_dim, NULL); y += dy;

  s_bright_idx = bright_step_for_nvs(BRIGHT_PCT, sizeof(BRIGHT_PCT) / sizeof(BRIGHT_PCT[0]));
  char b[8];
  snprintf(b, sizeof(b), "%u%%", (unsigned)BRIGHT_PCT[s_bright_idx]);
  s_bright_val = mk_row(page, t, y, "Brightness", b, t->ink_dim, on_bright_tap); y += dy;

  char thv[20]; snprintf(thv, sizeof(thv), "%s >", t->id ? t->id : "--");
  s_theme_val = mk_row(page, t, y, "Theme", thv, t->accent, on_theme_tap); y += dy;

  s_dim_val = mk_row(page, t, y, "Dim", "", t->ink_dim, dim_cb); y += dy;

  s_sleep_val = mk_row(page, t, y, "Sleep", "", t->ink_dim, sleep_cb); y += dy;

  mk_row(page, t, y, "About", ">", t->ink_dim, NULL);

  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  char wbuf[48]; net_status_str(wbuf, sizeof(wbuf)); lv_label_set_text_fmt(s_wifi_val, "%s >", wbuf);
  lv_label_set_text_fmt(s_theme_val, "%s >", t->id ? t->id : "--");

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

extern const screen_view_t settings_calm_view = { build, update };
