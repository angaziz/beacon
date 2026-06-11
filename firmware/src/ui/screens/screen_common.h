#pragma once
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include <stdarg.h>
#include "config/layout.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/screen.h"
#include "ui/theme.h"
#include "core/nvs.h"

// Diff-aware setters (#60). LVGL 8 reallocs+invalidates on every lv_label_set_text and refreshes+
// invalidates on every lv_obj_set_style_* / lv_bar_set_value regardless of whether the value changed,
// so a 500ms tick repaints the whole screen forever. Each helper reads the object's CURRENT value via
// an LVGL getter and only writes on a real change -- the object is its own cache, so nothing needs
// resetting when a view rebuilds on theme change. (lv_color_eq is LVGL 9; 8.4 compares .full.)
static inline bool color_eq(lv_color_t a, lv_color_t b) { return a.full == b.full; }
static inline void txt_set(lv_obj_t* l, const char* s) {
  const char* c = lv_label_get_text(l);
  if (c == NULL || strcmp(c, s) != 0) lv_label_set_text(l, s);
}
static inline void txt_fmt(lv_obj_t* l, const char* fmt, ...) {
  char b[64]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); txt_set(l, b);
}
static inline void txt_color(lv_obj_t* o, lv_color_t c) {
  if (!color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) lv_obj_set_style_text_color(o, c, 0);
}
static inline void bar_set_if(lv_obj_t* b, int32_t v) {
  if (lv_bar_get_value(b) != v) lv_bar_set_value(b, v, LV_ANIM_OFF);
}
static inline void hidden_set(lv_obj_t* o, bool on) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != on) {
    if (on) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}
static inline void bg_color_if(lv_obj_t* o, lv_color_t c, uint32_t part) {
  if (!color_eq(lv_obj_get_style_bg_color(o, part), c)) lv_obj_set_style_bg_color(o, c, part);
}
static inline void bg_opa_if(lv_obj_t* o, lv_opa_t a, uint32_t part) {
  if (lv_obj_get_style_bg_opa(o, part) != a) lv_obj_set_style_bg_opa(o, a, part);
}

// Settings views cache a brightness step index; snap it to the persisted backlight on (re)build so the
// shown step and the next tap match the restored value instead of a hardcoded default. Steps are PERCENT;
// 204 (=80%) is the boot default, matching the value main.cpp restores when the key is unset.
static inline uint8_t bright_step_for_nvs(const uint8_t* steps_pct, uint8_t n) {
  int raw = nvs_get_brightness(204), best = 0, bd = 1 << 30;
  for (uint8_t i = 0; i < n; i++) {
    int d = raw - (int)steps_pct[i] * 255 / 100; if (d < 0) d = -d;
    if (d < bd) { bd = d; best = i; }
  }
  return (uint8_t)best;
}

// Idempotent conditional style: remove first (no-op if absent) then add if on. Calling this
// every update() never accumulates duplicate style refs (LVGL add_style appends unconditionally).
static inline void style_set(lv_obj_t* o, lv_style_t* st, bool on) {
  lv_obj_remove_style(o, st, 0);
  if (on) lv_obj_add_style(o, st, 0);
}

// Top-right status slot: live header text, or the state chip (down-colored for severe states).
static inline void slot_set(lv_obj_t* slot, const char* live, const record_hdr_t* h, uint32_t now) {
  char chip[16];
  bool nonlive = sv_status(chip, sizeof(chip), h, now);
  txt_set(slot, nonlive ? chip : live);
  // #60: was style_set(&S.down) (a remove+add that invalidates every tick). S.down only sets
  // text_color, and the slot's base (S.slot) is ink_dim, so a diff-aware color set is equivalent.
  const beacon_theme_t* t = theme_active();
  if (t) txt_color(slot, (nonlive && sv_severe(h->state)) ? t->down : t->ink_dim);
}

// Dim a value label when its record is stale/offline/error (keeps last value, dimmed). Value labels
// carry S.display (ink); S.dim only sets text_color to ink_dim, so this matches the old style toggle.
static inline void value_state(lv_obj_t* lbl, screen_state_t s) {
  const beacon_theme_t* t = theme_active();
  if (t) txt_color(lbl, sv_dim(s) ? t->ink_dim : t->ink);
}

// Standard eyebrow ("BEACON / <id>") + a fixed-width right-aligned status slot (size-stable text).
static inline lv_obj_t* build_header(lv_obj_t* page, const char* id) {
  lv_obj_t* eb = lv_label_create(page);
  lv_obj_add_style(eb, &S.eyebrow, 0);
  lv_label_set_text_fmt(eb, "BEACON / %s", id);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_t* slot = lv_label_create(page);
  lv_obj_add_style(slot, &S.slot, 0);
  lv_obj_set_width(slot, 240);
  lv_obj_set_style_text_align(slot, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_text(slot, "");
  lv_obj_align(slot, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);
  return slot;
}
