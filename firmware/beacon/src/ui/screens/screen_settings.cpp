#include "ui/screens/screen_settings.h"
#include "ui/screens/screen_common.h"
#include "core/datastore.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "hal/display.h"

static lv_obj_t *s_theme_val, *s_bright_val, *s_tick_val;
static const uint8_t BRIGHT[] = {102, 153, 204, 255};   // 40/60/80/100%
static int s_bright_i = 2;

static lv_obj_t* row(lv_obj_t* page, const char* name, int y, lv_event_cb_t cb) {
  lv_obj_t* r = lv_obj_create(page); lv_obj_remove_style_all(r);
  lv_obj_set_size(r, SCREEN_W - 2*SAFE_INSET, 46); lv_obj_align(r, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30 + y);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* nm = lv_label_create(r); lv_obj_add_style(nm, &S.display, 0); lv_label_set_text(nm, name);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* val = lv_label_create(r); lv_obj_add_style(val, &S.slot, 0);
  lv_obj_set_width(val, 220); lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
  if (cb) { lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL); }
  lv_obj_t* rule = lv_obj_create(page); lv_obj_remove_style_all(rule); lv_obj_add_style(rule, &S.hairline, 0);
  lv_obj_set_size(rule, SCREEN_W - 2*SAFE_INSET, 1); lv_obj_align(rule, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 30 + y + 46);
  return val;
}

static void theme_cb(lv_event_t*) {
  theme_set((theme_index() + 1) % THEME_COUNT);
  lv_label_set_text(s_theme_val, theme_active()->id);
}
static void bright_cb(lv_event_t*) {
  s_bright_i = (s_bright_i + 1) % (int)(sizeof(BRIGHT)/sizeof(BRIGHT[0]));
  display_brightness(BRIGHT[s_bright_i]);
  lv_label_set_text_fmt(s_bright_val, "%d%%", (BRIGHT[s_bright_i] * 100 + 127) / 255);
}

static lv_obj_t* build(lv_obj_t* page) {
  build_header(page, "SETTINGS");
  lv_obj_t* wifi = row(page, "Wi-Fi", 0, NULL);             lv_label_set_text(wifi, "not set");
  s_bright_val   = row(page, "Brightness", 56, bright_cb);  lv_label_set_text(s_bright_val, "80%");
  s_theme_val    = row(page, "Theme", 112, theme_cb);       lv_obj_add_style(s_theme_val, &S.accent, 0);
  s_tick_val     = row(page, "Tickers", 168, NULL);
  lv_obj_t* slp  = row(page, "Sleep", 224, NULL);           lv_label_set_text(slp, "5 min");
  lv_obj_t* abt  = row(page, "About", 280, NULL);           lv_label_set_text(abt, ">");
  return page;
}

static void update(void) {
  if (theme_active()) lv_label_set_text(s_theme_val, theme_active()->id);
  lv_label_set_text_fmt(s_tick_val, "%d assets", ds_get_finance_count());
}

const screen_module_t settings_module = {"SETTINGS", build, update};
