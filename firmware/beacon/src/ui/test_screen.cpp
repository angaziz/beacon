#include "test_screen.h"
#include <lvgl.h>
#include "config/layout.h"
#include "util/log.h"

static lv_obj_t* s_label;
static int s_taps = 0;

static void btn_cb(lv_event_t* e) {
  s_taps++;
  lv_label_set_text_fmt(s_label, "taps: %d", s_taps);
  LOGI("button tap %d", s_taps);
}

void test_screen_show() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  // Safe-area container: everything lives inside SAFE_INSET.
  lv_obj_t* safe = lv_obj_create(scr);
  lv_obj_set_size(safe, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET);
  lv_obj_center(safe);
  lv_obj_set_style_bg_opa(safe, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(safe, 1, 0);
  lv_obj_set_style_border_color(safe, lv_color_hex(0x333333), 0);

  s_label = lv_label_create(safe);
  lv_label_set_text(s_label, "BEACON P0-A");
  lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
  lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* btn = lv_btn_create(safe);
  lv_obj_set_size(btn, 200, 80);     // >= 64px touch target (DESIGN.md)
  lv_obj_center(btn);
  lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, "TAP ME");
  lv_obj_center(bl);
}
