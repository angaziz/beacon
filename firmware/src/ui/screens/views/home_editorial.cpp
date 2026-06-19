#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "ui/screens/views/view_common.h"
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
  render_clock_ex(s_clock, s_date, "%a %d %b", txt_set);
  weather_rec_t w = ds_get_weather(); uint32_t now = now_s();
  slot_set(s_slot, "Wnn . --", &w.hdr, now);
  if (sv_placeholder(w.hdr.state)) { txt_set(s_temp, "--"); txt_set(s_hum, "--"); }
  else {
    // libc snprintf (not lv_label_set_text_fmt): LVGL's sprintf has LV_SPRINTF_USE_FLOAT=0, so %f
    // would render a literal "f". Matches the other home views.
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", w.temp_c);   txt_set(s_temp, buf);
    snprintf(buf, sizeof(buf), "%.0f%%", w.humidity_pct);   txt_set(s_hum, buf);
  }
  value_state(s_temp, w.hdr.state); value_state(s_hum, w.hdr.state);
}

extern const screen_view_t home_editorial_view = { build, update };
