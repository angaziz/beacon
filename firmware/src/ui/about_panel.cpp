#include "ui/about_panel.h"
#include "ui/theme.h"
#include "ui/carousel.h"
#include "config/layout.h"
#include "config/version.h"
#include "core/about_format.h"
#include <lvgl.h>
#include <Arduino.h>
#include <esp_mac.h>
#include <esp_system.h>

static lv_obj_t* s_root = nullptr;   // full-screen modal on lv_layer_top

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

// Deferred: a tap on the root dismisses, but deleting s_root from inside its own
// event is unsafe (mirrors theme_panel's apply_pick deferral).
static void close_async(void*) { close_panel(); }

static void back_cb(lv_event_t*) { close_panel(); }   // target is a child label -- safe to close inline
static void root_cb(lv_event_t*) { lv_async_call(close_async, NULL); }

static void add_row(lv_obj_t* list, const beacon_theme_t* t, const char* name, const char* value) {
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 40);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* nm = lv_label_create(row); mklabel(nm, t->f_body, t->ink_dim);
  lv_label_set_text(nm, name); lv_obj_align(nm, LV_ALIGN_LEFT_MID, 2, 0);

  lv_obj_t* val = lv_label_create(row); mklabel(val, t->f_mono, t->ink);
  lv_obj_set_width(val, SCREEN_W - 2 * SAFE_INSET - 90);
  lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(val, LV_ALIGN_RIGHT_MID, -2, 0);
  lv_label_set_text(val, value);
}

void about_panel_open(void) {
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
  lv_obj_add_event_cb(s_root, root_cb, LV_EVENT_CLICKED, NULL);   // tap anywhere dismisses

  lv_obj_t* back = lv_label_create(s_root); mklabel(back, t->f_body, t->ink_dim);
  lv_label_set_text(back, "< back"); lv_obj_align(back, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(back, 24);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* title = lv_label_create(s_root); mklabel(title, t->f_body, t->ink_dim);
  lv_label_set_text(title, "ABOUT"); lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* list = lv_obj_create(s_root);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(list, 2, 0);
  lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);   // taps fall through to root => dismiss

  uint8_t wmac[6] = {0}, bmac[6] = {0};
  esp_read_mac(wmac, ESP_MAC_WIFI_STA);
  esp_read_mac(bmac, ESP_MAC_BT);
  char wbuf[18], bbuf[18], up[16], heap[16];
  about_fmt_mac(wmac, wbuf);
  about_fmt_mac(bmac, bbuf);
  about_fmt_uptime(millis() / 1000, up, sizeof(up));
  about_fmt_heap_kb(esp_get_free_heap_size(), heap, sizeof(heap));

  add_row(list, t, "VERSION", FIRMWARE_VERSION);
  add_row(list, t, "BUILD",   __DATE__);
  add_row(list, t, "WIFI",    wbuf);
  add_row(list, t, "BLE",     bbuf);
  add_row(list, t, "UPTIME",  up);
  add_row(list, t, "HEAP",    heap);
}

bool about_panel_is_open(void) { return s_root != nullptr; }

void about_panel_close(void) { close_panel(); }
