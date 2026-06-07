#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// Analog Neo now-playing: minimal ice-blue. Device in the header. Title (display) + artist (dim).
// A thin progress rule (bar) = progress_ms/duration_ms with elapsed/total times and a playing/
// paused mark beneath. has_device==false => "no active device". Controls are P4 placeholders.

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

static lv_obj_t *s_status, *s_title, *s_artist, *s_bar, *s_elapsed, *s_total, *s_play, *s_empty;

static void fmt_mmss(char* buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "now playing");
  lv_obj_set_style_text_font(eyebrow, t->f_mono, 0);
  lv_obj_set_style_text_color(eyebrow, t->ink_dim, 0);
  lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "--");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_title = lv_label_create(page);
  lv_label_set_text(s_title, "--");
  lv_obj_set_style_text_font(s_title, t->f_display, 0);
  lv_obj_set_style_text_color(s_title, t->ink, 0);
  lv_obj_set_width(s_title, SCREEN_W - 2 * SAFE_INSET);
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -20);

  s_artist = lv_label_create(page);
  lv_label_set_text(s_artist, "--");
  lv_obj_set_style_text_font(s_artist, t->f_mono, 0);
  lv_obj_set_style_text_color(s_artist, t->ink_dim, 0);
  lv_obj_set_width(s_artist, SCREEN_W - 2 * SAFE_INSET);
  lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
  lv_obj_align(s_artist, LV_ALIGN_CENTER, 0, 14);

  // Thin progress rule.
  s_bar = lv_bar_create(page);
  lv_obj_set_size(s_bar, SCREEN_W - 2 * SAFE_INSET, 3);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 28));
  lv_bar_set_range(s_bar, 0, 1000);
  lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bar, t->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, t->accent, LV_PART_INDICATOR);

  s_elapsed = lv_label_create(page);
  lv_label_set_text(s_elapsed, "-:--");
  lv_obj_set_style_text_font(s_elapsed, t->f_mono, 0);
  lv_obj_set_style_text_color(s_elapsed, t->ink_dim, 0);
  lv_obj_align(s_elapsed, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);

  s_play = lv_label_create(page);
  lv_label_set_text(s_play, "--");
  lv_obj_set_style_text_font(s_play, t->f_mono, 0);
  lv_obj_set_style_text_color(s_play, t->accent, 0);
  lv_obj_align(s_play, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  s_total = lv_label_create(page);
  lv_label_set_text(s_total, "-:--");
  lv_obj_set_style_text_font(s_total, t->f_mono, 0);
  lv_obj_set_style_text_color(s_total, t->ink_dim, 0);
  lv_obj_align(s_total, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);

  // "no active device" overlay, hidden by default.
  s_empty = lv_label_create(page);
  lv_label_set_text(s_empty, "no active device");
  lv_obj_set_style_text_font(s_empty, t->f_mono, 0);
  lv_obj_set_style_text_color(s_empty, t->ink_dim, 0);
  lv_obj_center(s_empty);
  lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
}

static void show_track(bool on) {
  lv_obj_t* p[] = { s_title, s_artist, s_bar, s_elapsed, s_play, s_total };
  for (lv_obj_t* o : p) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  if (on) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
  else    lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  nowplaying_rec_t n = ds_get_nowplaying();
  uint32_t now = now_s();

  char chip[24];
  if (sv_status(chip, sizeof(chip), &n.hdr, now)) {
    lv_label_set_text(s_status, chip);
    lv_obj_set_style_text_color(s_status, sv_severe(n.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, n.has_device ? n.device : "--");
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  if (!n.has_device) { show_track(false); return; }
  show_track(true);

  lv_color_t vcol = sv_dim(n.hdr.state) ? t->ink_dim : t->ink;
  if (sv_placeholder(n.hdr.state)) {
    lv_label_set_text(s_title, "--");
    lv_label_set_text(s_artist, "--");
  } else {
    lv_label_set_text(s_title, n.title);
    lv_label_set_text(s_artist, n.artist);
  }
  lv_obj_set_style_text_color(s_title, vcol, 0);

  uint16_t frac = 0;
  if (n.duration_ms > 0) {
    uint32_t f = (uint64_t)n.progress_ms * 1000 / n.duration_ms;
    frac = f > 1000 ? 1000 : (uint16_t)f;
  }
  lv_bar_set_value(s_bar, frac, LV_ANIM_OFF);

  char eb[8]; fmt_mmss(eb, sizeof(eb), n.progress_ms); lv_label_set_text(s_elapsed, eb);
  char tb[8]; fmt_mmss(tb, sizeof(tb), n.duration_ms); lv_label_set_text(s_total, tb);
  lv_label_set_text(s_play, n.playing ? "playing" : "paused");
  lv_obj_set_style_text_color(s_play, n.playing ? t->accent : t->ink_dim, 0);
}

extern const screen_view_t nowplaying_analog_view = { build, update };
