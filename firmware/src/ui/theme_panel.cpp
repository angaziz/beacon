#include "ui/theme_panel.h"
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/carousel.h"
#include "config/layout.h"
#include <lvgl.h>
#include <ctype.h>

static lv_obj_t* s_root = nullptr;   // full-screen modal on lv_layer_top
static uint8_t   s_pick = 0;         // theme tapped this frame, applied from the async tail

static void mklabel(lv_obj_t* o, const lv_font_t* f, lv_color_t c) {
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_set_style_text_color(o, c, 0);
  lv_obj_set_style_text_letter_space(o, 2, 0);
}

static void close_panel(void) {
  if (!s_root) return;
  lv_obj_del(s_root);
  s_root = nullptr;
  carousel_set_swipe_enabled(true);   // unconditional restore -- never leave the carousel frozen
}

// Deferred so we don't apply + delete the panel from inside the tapped row's own event: theme_set
// rebuilds the carousel pages (not this panel) and close_panel frees s_root, both unsafe mid-dispatch.
static void apply_pick(void*) {
  theme_set(s_pick);
  close_panel();
}

static void back_cb(lv_event_t*) { close_panel(); }
static void row_cb(lv_event_t* e) {
  s_pick = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  lv_async_call(apply_pick, NULL);
}

void theme_panel_open(void) {
  if (s_root) return;                 // already open (idempotent)
  const beacon_theme_t* t = theme_active();
  carousel_set_swipe_enabled(false);

  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
  lv_obj_center(s_root);
  lv_obj_set_style_bg_color(s_root, t->bg, 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);   // capture taps so none leak to the carousel

  lv_obj_t* back = lv_label_create(s_root); mklabel(back, t->f_body, t->ink_dim);
  lv_label_set_text(back, "< back"); lv_obj_align(back, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(back, 24);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* title = lv_label_create(s_root); mklabel(title, t->f_body, t->ink_dim);
  lv_label_set_text(title, "THEME"); lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* list = lv_obj_create(s_root);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(list, 2, 0);

  uint8_t active = theme_index();
  for (uint8_t i = 0; i < THEME_COUNT; i++) {
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
    lv_obj_set_style_bg_color(dot, i == active ? t->accent : t->line, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);

    char nm[20]; const char* id = THEME_CATALOG[i].id; uint8_t k = 0;   // catalog ids are lowercase
    for (; id[k] && k < sizeof(nm) - 1; k++) nm[k] = (char)toupper((unsigned char)id[k]);
    nm[k] = 0;
    lv_obj_t* lbl = lv_label_create(row); mklabel(lbl, t->f_display, i == active ? t->accent : t->ink);
    lv_label_set_text(lbl, nm); lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
  }
}

void theme_panel_close(void) { close_panel(); }
bool theme_panel_is_open(void) { return s_root != nullptr; }
