#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "hal/display.h"
#include "hal/power.h"
#include <Arduino.h>

// Analog Neo settings: minimal ice-blue rows (label left display, value right mono, hairline
// rule). Theme row taps cycle theme (deferred via lv_async_call -- theme_set deletes this object
// mid-event). Brightness row taps cycle 40/60/80/100% (display_brightness inline).

static lv_obj_t *s_theme_val, *s_bright_val, *s_tickers_val, *s_batt_val;

static const uint8_t BRIGHT_PCT[] = { 40, 60, 80, 100 };
static uint8_t s_bright_idx = 2;   // 80%

static void do_next_theme(void*) { theme_set((theme_index() + 1) % THEME_COUNT); }

static void theme_tap_cb(lv_event_t*) { lv_async_call(do_next_theme, NULL); }

static void bright_tap_cb(lv_event_t*) {
  s_bright_idx = (uint8_t)((s_bright_idx + 1) % (sizeof(BRIGHT_PCT) / sizeof(BRIGHT_PCT[0])));
  uint8_t pct = BRIGHT_PCT[s_bright_idx];
  display_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  char b[8]; snprintf(b, sizeof(b), "%u%%", pct);
  lv_label_set_text(s_bright_val, b);
}

static lv_obj_t* make_row(lv_obj_t* parent, const beacon_theme_t* t, int y,
                          const char* name, const char* val, bool accent_val,
                          lv_event_cb_t tap) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, SCREEN_W - 2 * SAFE_INSET, 46);
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
  const int pitch = 48;
  char bb[8]; snprintf(bb, sizeof(bb), "%u%%", BRIGHT_PCT[s_bright_idx]);
  char tk[24]; snprintf(tk, sizeof(tk), "%u assets >", (unsigned)ds_get_finance_count());

  make_row(page, t, top + 0 * pitch, "wi-fi", "not set", false, NULL);
  s_batt_val    = make_row(page, t, top + 1 * pitch, "battery", "--", false, NULL);
  s_bright_val  = make_row(page, t, top + 2 * pitch, "brightness", bb, false, bright_tap_cb);
  s_theme_val   = make_row(page, t, top + 3 * pitch, "theme",
                           THEME_CATALOG[theme_index()].id, true, theme_tap_cb);
  s_tickers_val = make_row(page, t, top + 4 * pitch, "tickers", tk, false, NULL);
  make_row(page, t, top + 5 * pitch, "sleep", "5 min", false, NULL);
  make_row(page, t, top + 6 * pitch, "about", ">", false, NULL);
}

static void update(void) {
  lv_label_set_text(s_theme_val, THEME_CATALOG[theme_index()].id);
  char tk[24]; snprintf(tk, sizeof(tk), "%u assets >", (unsigned)ds_get_finance_count());
  lv_label_set_text(s_tickers_val, tk);

  int pct = power_battery_pct();
  char bt[8];
  if (pct >= 0) snprintf(bt, sizeof(bt), "%d%%%s", pct, power_charging() ? "+" : "");
  else          snprintf(bt, sizeof(bt), "%s", power_charging() ? "USB" : "--");
  lv_label_set_text(s_batt_val, bt);
    lv_obj_set_style_text_color(s_batt_val, power_charging() ? theme_active()->accent : (pct >= 0 && pct <= 20 ? theme_active()->down : theme_active()->ink), 0);
}

extern const screen_view_t settings_analog_view = { build, update };
