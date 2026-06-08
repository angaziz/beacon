#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "core/datastore.h"
#include "ui/theme.h"
#include "ui/theme_panel.h"
#include "hal/display.h"
#include "hal/power.h"
#include "core/net.h"
#include "core/nvs.h"
#include "ui/wifi_panel.h"

static lv_obj_t *s_theme_val, *s_bright_val, *s_tick_val, *s_batt_val, *s_wifi_val;
static const uint8_t BRIGHT[] = {102, 153, 204, 255};   // 40/60/80/100%
static int s_bright_i = 2;

static void theme_cb(lv_event_t*) { theme_panel_open(); }
static void wifi_open_cb(lv_event_t*) { wifi_panel_open(); }
static void bright_cb(lv_event_t*) {
  s_bright_i = (s_bright_i + 1) % (int)(sizeof(BRIGHT)/sizeof(BRIGHT[0]));
  display_brightness(BRIGHT[s_bright_i]);
  nvs_set_brightness(BRIGHT[s_bright_i]);
  lv_label_set_text_fmt(s_bright_val, "%d%%", (BRIGHT[s_bright_i] * 100 + 127) / 255);
}

static lv_obj_t* row(lv_obj_t* page, const char* name, int y, lv_event_cb_t cb) {
  lv_obj_t* r = lv_obj_create(page); lv_obj_remove_style_all(r);
  lv_obj_set_size(r, SCREEN_W - 2*SAFE_INSET, 44); lv_obj_align(r, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30 + y);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* nm = lv_label_create(r); lv_obj_add_style(nm, &S.display, 0); lv_label_set_text(nm, name);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* val = lv_label_create(r); lv_obj_add_style(val, &S.slot, 0);
  lv_obj_set_width(val, 220); lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
  if (cb) { lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL); }
  lv_obj_t* rule = lv_obj_create(page); lv_obj_remove_style_all(rule); lv_obj_add_style(rule, &S.hairline, 0);
  lv_obj_set_size(rule, SCREEN_W - 2*SAFE_INSET, 1); lv_obj_align(rule, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30 + y + 44);
  return val;
}

static void build(lv_obj_t* page) {
  build_header(page, "SETTINGS");
  s_wifi_val     = row(page, "Wi-Fi", 0, NULL);             lv_label_set_text(s_wifi_val, "not set >");
  lv_obj_t* wrow = lv_obj_get_parent(s_wifi_val); lv_obj_add_flag(wrow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wrow, wifi_open_cb, LV_EVENT_CLICKED, NULL);
  s_batt_val     = row(page, "Battery", 50, NULL);          lv_label_set_text(s_batt_val, "--");
  { uint8_t raw = nvs_get_brightness(204); int bd = 1 << 30;   // snap step to restored backlight (BRIGHT is raw)
    for (int i = 0; i < (int)(sizeof(BRIGHT) / sizeof(BRIGHT[0])); i++) {
      int d = (int)raw - (int)BRIGHT[i]; if (d < 0) d = -d;
      if (d < bd) { bd = d; s_bright_i = i; } } }
  s_bright_val   = row(page, "Brightness", 100, bright_cb); lv_label_set_text_fmt(s_bright_val, "%d%%", (BRIGHT[s_bright_i]*100+127)/255);
  s_theme_val    = row(page, "Theme", 150, theme_cb);       lv_obj_add_style(s_theme_val, &S.accent, 0);
  s_tick_val     = row(page, "Tickers", 200, NULL);
  lv_obj_t* slp  = row(page, "Sleep", 250, NULL);           lv_label_set_text(slp, "5 min");
  lv_obj_t* abt  = row(page, "About", 300, NULL);           lv_label_set_text(abt, ">");
}

static void update(void) {
  char wbuf[48]; net_status_str(wbuf, sizeof(wbuf)); lv_label_set_text_fmt(s_wifi_val, "%s >", wbuf);
  if (theme_active()) lv_label_set_text_fmt(s_theme_val, "%s >", theme_active()->id);
  lv_label_set_text_fmt(s_tick_val, "%d assets", ds_get_finance_count());

  int pct = power_battery_pct();
  if (pct >= 0) lv_label_set_text_fmt(s_batt_val, "%d%%%s", pct, power_charging() ? "+" : "");
  else          lv_label_set_text(s_batt_val, power_charging() ? "USB" : "--");
  lv_obj_set_style_text_color(s_batt_val, power_charging() ? theme_active()->accent
    : (pct >= 0 && pct <= 20 ? theme_active()->down : theme_active()->ink), 0);
}

extern const screen_view_t settings_editorial_view = { build, update };
