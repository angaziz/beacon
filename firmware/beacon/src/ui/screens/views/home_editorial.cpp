#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "core/datastore.h"

static lv_obj_t *s_slot, *s_clock, *s_date, *s_temp, *s_hum;

static void build(lv_obj_t* page) {
  s_slot = build_header(page, "HOME");
  s_clock = lv_label_create(page); lv_obj_add_style(s_clock, &S.hero, 0);
  lv_label_set_text(s_clock, "--:--"); lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, SAFE_INSET, -30);
  s_date = lv_label_create(page); lv_obj_add_style(s_date, &S.slot, 0);
  lv_label_set_text(s_date, "--"); lv_obj_align(s_date, LV_ALIGN_LEFT_MID, SAFE_INSET, 30);
  lv_obj_t* rule = lv_obj_create(page); lv_obj_remove_style_all(rule);
  lv_obj_add_style(rule, &S.hairline, 0); lv_obj_set_size(rule, SCREEN_W - 2*SAFE_INSET, 1);
  lv_obj_align(rule, LV_ALIGN_LEFT_MID, SAFE_INSET, 64);
  s_temp = lv_label_create(page); lv_obj_add_style(s_temp, &S.display, 0);
  lv_obj_align(s_temp, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET - 24);
  s_hum = lv_label_create(page); lv_obj_add_style(s_hum, &S.display, 0);
  lv_obj_align(s_hum, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET + 160, -SAFE_INSET - 24);
  lv_obj_t* tl = lv_label_create(page); lv_obj_add_style(tl, &S.slot, 0);
  lv_label_set_text(tl, "TEMP"); lv_obj_align(tl, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);
  lv_obj_t* hl = lv_label_create(page); lv_obj_add_style(hl, &S.slot, 0);
  lv_label_set_text(hl, "HUMIDITY"); lv_obj_align(hl, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET + 160, -SAFE_INSET);
}

static void update(void) {
  weather_rec_t w = ds_get_weather(); uint32_t now = now_s();
  slot_set(s_slot, "Wnn . --", &w.hdr, now);
  if (sv_placeholder(w.hdr.state)) { lv_label_set_text(s_temp, "--"); lv_label_set_text(s_hum, "--"); }
  else {
    lv_label_set_text_fmt(s_temp, "%.1f\xC2\xB0", w.temp_c);
    lv_label_set_text_fmt(s_hum, "%.0f%%", w.humidity_pct);
  }
  value_state(s_temp, w.hdr.state); value_state(s_hum, w.hdr.state);
}

extern const screen_view_t home_editorial_view = { build, update };
