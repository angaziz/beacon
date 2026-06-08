#include "ui/pair_overlay.h"
#include "core/hublink_ble.h"
#include "ui/theme.h"
#include "ui/screen.h"      // SCREEN_W / safe-area consts via layout
#include <lvgl.h>
#include <stdio.h>

static lv_obj_t* s_card = nullptr;

static void show(uint32_t passkey) {
  if (s_card) return;
  const beacon_theme_t* t = theme_active();
  lv_obj_t* card = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 320, LV_SIZE_CONTENT);
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
  lv_obj_set_style_pad_row(card, 10, 0);

  lv_obj_t* eb = lv_label_create(card);
  lv_obj_set_style_text_color(eb, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(eb, 3, 0);
  lv_label_set_text(eb, "PAIR WITH HUB");

  lv_obj_t* code = lv_label_create(card);
  lv_obj_set_style_text_color(code, t->accent, 0);
  if (t->f_hero) lv_obj_set_style_text_font(code, t->f_hero, 0);   // digit-subset hero font
  char b[8]; snprintf(b, sizeof(b), "%06u", (unsigned)passkey);
  lv_label_set_text(code, b);

  lv_obj_t* hint = lv_label_create(card);
  lv_obj_set_width(hint, 280);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(hint, t->ink, 0);
  lv_label_set_text(hint, "Enter this code on your Mac to pair.");

  s_card = card;
}

static void hide(void) {
  if (!s_card) return;
  lv_obj_del(s_card);
  s_card = nullptr;
}

void pair_overlay_service(void) {
  if (hublink_ble_is_pairing()) show(hublink_ble_passkey());
  else hide();
}
