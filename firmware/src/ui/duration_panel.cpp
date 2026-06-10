#include "ui/duration_panel.h"
#include "ui/durations.h"
#include "ui/theme.h"
#include "ui/carousel.h"
#include "config/layout.h"
#include <lvgl.h>

static lv_obj_t*        s_root  = nullptr;
static duration_pick_cb s_cb    = nullptr;
static uint8_t          s_pick  = 0;

static void mklabel(lv_obj_t* o, const lv_font_t* f, lv_color_t c) {
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_set_style_text_color(o, c, 0);
  lv_obj_set_style_text_letter_space(o, 2, 0);
}

void duration_panel_close(void) {
  if (!s_root) return;
  lv_obj_del(s_root);
  s_root = nullptr;
  s_cb = nullptr;
  carousel_set_swipe_enabled(true);
}

// Deferred: applying from inside the tapped row's own event while we also free s_root is unsafe.
static void apply_pick(void*) {
  duration_pick_cb cb = s_cb;
  uint8_t idx = s_pick;
  duration_panel_close();
  if (cb) cb(idx);
}

static void back_cb(lv_event_t*) { duration_panel_close(); }
static void row_cb(lv_event_t* e) {
  s_pick = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  lv_async_call(apply_pick, NULL);
}

void duration_panel_open(const char* title, uint8_t current, duration_pick_cb on_pick) {
  if (s_root) return;
  const beacon_theme_t* t = theme_active();
  s_cb = on_pick;
  carousel_set_swipe_enabled(false);

  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
  lv_obj_center(s_root);
  lv_obj_set_style_bg_color(s_root, t->bg, 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* back = lv_label_create(s_root); mklabel(back, t->f_body, t->ink_dim);
  lv_label_set_text(back, "< back"); lv_obj_align(back, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(back, 24);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* ttl = lv_label_create(s_root); mklabel(ttl, t->f_body, t->ink_dim);
  lv_label_set_text(ttl, title); lv_obj_align(ttl, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* list = lv_obj_create(s_root);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(list, 2, 0);

  for (uint8_t i = 0; i < DURATION_COUNT; i++) {
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, i == current ? t->accent : t->line, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t* lbl = lv_label_create(row);
    mklabel(lbl, t->f_display, i == current ? t->accent : t->ink);
    lv_label_set_text(lbl, DURATIONS[i].label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
  }
}

bool duration_panel_is_open(void) { return s_root != nullptr; }
