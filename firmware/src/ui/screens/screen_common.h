#pragma once
#include <lvgl.h>
#include <Arduino.h>
#include "config/layout.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/screen.h"
#include "core/nvs.h"

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
  lv_label_set_text(slot, nonlive ? chip : live);
  style_set(slot, &S.down, nonlive && sv_severe(h->state));
}

// Dim a value label when its record is stale/offline/error (keeps last value, dimmed).
static inline void value_state(lv_obj_t* lbl, screen_state_t s) { style_set(lbl, &S.dim, sv_dim(s)); }

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
