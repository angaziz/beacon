#include "ui/brightness_overlay.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "hal/display.h"
#include "core/nvs.h"
#include <lvgl.h>

static const uint32_t AUTOCLOSE_MS = 4000;   // close after this much idle on the overlay
static const int32_t  BRIGHT_MIN   = 26;     // ~10%: never let the quick control blank the panel

static lv_obj_t* s_root = nullptr;   // dimmed full-screen backdrop (tap-out target)
static lv_obj_t* s_val  = nullptr;   // "%" readout

static void apply(uint8_t v) {
  display_brightness(v);
  nvs_set_brightness(v);
  if (s_val) lv_label_set_text_fmt(s_val, "%d%%", (v * 100 + 127) / 255);
}

static void slider_cb(lv_event_t* e) {
  apply((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}
static void backdrop_cb(lv_event_t* e) {   // a click on the backdrop (outside the card) dismisses
  if (lv_event_get_target(e) == s_root) brightness_overlay_close();
}

void brightness_overlay_open(void) {
  if (s_root) return;
  const beacon_theme_t* t = theme_active();

  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
  lv_obj_center(s_root);
  lv_obj_set_style_bg_color(s_root, t->bg, 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_70, 0);   // dim the screen behind
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_root, backdrop_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* card = lv_obj_create(s_root);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 300, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, t->bg, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, t->accent, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 22, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card, 14, 0);

  lv_obj_t* eb = lv_label_create(card);
  lv_obj_set_style_text_color(eb, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(eb, 3, 0);
  lv_label_set_text(eb, "BRIGHTNESS");

  s_val = lv_label_create(card);
  lv_obj_set_style_text_color(s_val, t->accent, 0);
  if (t->f_hero) lv_obj_set_style_text_font(s_val, t->f_hero, 0);   // digit-subset hero font

  lv_obj_t* sl = lv_slider_create(card);
  lv_obj_set_width(sl, 240);
  lv_slider_set_range(sl, BRIGHT_MIN, 255);
  uint8_t cur = nvs_get_brightness(204);
  if (cur < BRIGHT_MIN) cur = BRIGHT_MIN;
  lv_slider_set_value(sl, cur, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, t->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, t->accent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, t->accent, LV_PART_KNOB);
  lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_label_set_text_fmt(s_val, "%d%%", (cur * 100 + 127) / 255);
  lv_disp_trig_activity(NULL);   // opening counts as activity; start the autoclose clock fresh
}

void brightness_overlay_close(void) {
  if (!s_root) return;
  lv_obj_del(s_root);
  s_root = nullptr;
  s_val = nullptr;
}

bool brightness_overlay_is_open(void) { return s_root != nullptr; }

void brightness_overlay_service(void) {
  if (s_root && lv_disp_get_inactive_time(NULL) > AUTOCLOSE_MS) brightness_overlay_close();
}
