#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/theme_panel.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "hal/display.h"
#include "hal/power.h"
#include "core/net.h"
#include "core/nvs.h"
#include "ui/screens/screen_common.h"
#include "ui/wifi_panel.h"
#include <Arduino.h>
#include <ctype.h>

// LED Matrix / SETTINGS: lit-label rows. Theme tap opens the theme picker (theme_panel).
// Brightness tap cycles 40/60/80/100% inline.

static lv_obj_t *s_theme_val, *s_bright_val, *s_tickers_val, *s_batt_val, *s_wifi_val;
static const uint8_t BRIGHT_STEPS[] = { 40, 60, 80, 100 };
static uint8_t s_bright_idx = 2;  // default 80%

static void theme_cb(lv_event_t*) { theme_panel_open(); }

static void wifi_open_cb(lv_event_t*) { wifi_panel_open(); }

static void bright_cb(lv_event_t*) {
  s_bright_idx = (s_bright_idx + 1) % (sizeof(BRIGHT_STEPS) / sizeof(BRIGHT_STEPS[0]));
  uint8_t pct = BRIGHT_STEPS[s_bright_idx];
  display_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  nvs_set_brightness((uint8_t)((uint16_t)pct * 255 / 100));
  char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
  lv_label_set_text(s_bright_val, buf);
}

static lv_obj_t* make_row(lv_obj_t* parent, const beacon_theme_t* t, const char* name,
                          const char* val, lv_color_t vc) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* nm = lv_label_create(row);
  lv_label_set_text(nm, name);
  lv_obj_set_style_text_font(nm, t->f_mono, 0);
  lv_obj_set_style_text_color(nm, t->accent, 0);

  lv_obj_t* v = lv_label_create(row);
  lv_label_set_text(v, val);
  lv_obj_set_style_text_font(v, t->f_mono, 0);
  lv_obj_set_style_text_color(v, vc, 0);

  lv_obj_set_user_data(row, v);  // expose value label to caller
  return row;
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;

  lv_obj_t* eb = lv_label_create(page);
  lv_label_set_text(eb, "BEACON / SETTINGS");
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->accent, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  lv_obj_t* ver = lv_label_create(page);
  lv_label_set_text(ver, "V0.1");
  lv_obj_set_style_text_font(ver, t->f_mono, 0);
  lv_obj_set_style_text_color(ver, t->ink_dim, 0);
  lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* list = lv_obj_create(page);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* wf = make_row(list, t, "WI-FI", "NOT SET >", t->ink_dim);
  s_wifi_val = (lv_obj_t*)lv_obj_get_user_data(wf);
  lv_obj_add_flag(wf, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wf, wifi_open_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* bt = make_row(list, t, "BATTERY", "--", t->ink);
  s_batt_val = (lv_obj_t*)lv_obj_get_user_data(bt);

  s_bright_idx = bright_step_for_nvs(BRIGHT_STEPS, sizeof(BRIGHT_STEPS) / sizeof(BRIGHT_STEPS[0]));
  char buf[12];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)BRIGHT_STEPS[s_bright_idx]);
  lv_obj_t* br = make_row(list, t, "BRIGHTNESS", buf, t->ink);
  s_bright_val = (lv_obj_t*)lv_obj_get_user_data(br);
  lv_obj_add_flag(br, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(br, bright_cb, LV_EVENT_CLICKED, NULL);

  char thv[20]; snprintf(thv, sizeof(thv), "%s >", t->id);
  lv_obj_t* th = make_row(list, t, "THEME", thv, t->accent);
  s_theme_val = (lv_obj_t*)lv_obj_get_user_data(th);
  lv_obj_add_flag(th, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(th, theme_cb, LV_EVENT_CLICKED, NULL);

  char tk[20];
  snprintf(tk, sizeof(tk), "%u ASSETS >", (unsigned)ds_get_finance_count());
  lv_obj_t* tr = make_row(list, t, "TICKERS", tk, t->ink_dim);
  s_tickers_val = (lv_obj_t*)lv_obj_get_user_data(tr);

  make_row(list, t, "SLEEP", "5 MIN", t->ink);
  make_row(list, t, "ABOUT", ">", t->ink_dim);
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;
  if (s_wifi_val) {
    char wbuf[48]; net_status_str(wbuf, sizeof(wbuf)); lv_label_set_text_fmt(s_wifi_val, "%s >", wbuf);
  }
  if (s_theme_val) {
    char id[16];
    snprintf(id, sizeof(id), "%s", t->id);
    for (char* p = id; *p; p++) *p = (char)toupper((unsigned char)*p);
    lv_label_set_text_fmt(s_theme_val, "%s >", id);
  }
  if (s_tickers_val) {
    char tk[20];
    snprintf(tk, sizeof(tk), "%u ASSETS >", (unsigned)ds_get_finance_count());
    lv_label_set_text(s_tickers_val, tk);
  }
  if (s_batt_val) {
    int pct = power_battery_pct();
    char bt[8];
    if (pct >= 0) snprintf(bt, sizeof(bt), "%d%%%s", pct, power_charging() ? "+" : "");
    else          snprintf(bt, sizeof(bt), "%s", power_charging() ? "USB" : "--");
    lv_label_set_text(s_batt_val, bt);
    lv_obj_set_style_text_color(s_batt_val, power_charging() ? theme_active()->accent : (pct >= 0 && pct <= 20 ? theme_active()->down : theme_active()->ink), 0);
  }
}

extern const screen_view_t settings_led_view = { build, update };
