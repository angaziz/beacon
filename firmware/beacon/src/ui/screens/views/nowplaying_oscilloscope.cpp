#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// Oscilloscope / Signal NOW. Scope-instrument language: source readout in the header, title
// as the bright phosphor figure, artist dim, a glowing progress trace (lv_bar) flanked by
// elapsed/duration, transport PLAYING/PAUSED. Controls are non-functional placeholders.

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

static lv_obj_t *s_src, *s_title, *s_artist;
static lv_obj_t *s_bar, *s_elapsed, *s_remain, *s_state;

static void update(void);

static void fmt_ms(char* buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* hdr = lv_label_create(page);
  lv_label_set_text(hdr, "NOW . SIGNAL IN");
  lv_obj_set_style_text_color(hdr, t->ink_dim, 0);
  lv_obj_set_style_text_font(hdr, t->f_mono, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_src = lv_label_create(page);
  lv_label_set_text(s_src, "> --");
  lv_obj_set_style_text_color(s_src, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_src, t->f_mono, 0);
  lv_obj_align(s_src, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_title = lv_label_create(page);
  lv_label_set_text(s_title, "--");
  lv_obj_set_style_text_color(s_title, t->ink, 0);
  lv_obj_set_style_text_font(s_title, t->f_display, 0);
  lv_obj_set_width(s_title, SCREEN_W - 2 * SAFE_INSET);
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_align(s_title, LV_ALIGN_LEFT_MID, SAFE_INSET, -24);

  s_artist = lv_label_create(page);
  lv_label_set_text(s_artist, "--");
  lv_obj_set_style_text_color(s_artist, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_artist, t->f_mono, 0);
  lv_obj_set_width(s_artist, SCREEN_W - 2 * SAFE_INSET);
  lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
  lv_obj_align(s_artist, LV_ALIGN_LEFT_MID, SAFE_INSET, 20);

  // Progress trace: phosphor fill on a dim track.
  s_bar = lv_bar_create(page);
  lv_obj_set_size(s_bar, SCREEN_W - 2 * SAFE_INSET, 4);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 30));
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
  lv_obj_set_style_text_color(s_elapsed, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_elapsed, t->f_mono, 0);
  lv_obj_align(s_elapsed, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);

  s_remain = lv_label_create(page);
  lv_label_set_text(s_remain, "-:--");
  lv_obj_set_style_text_color(s_remain, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_remain, t->f_mono, 0);
  lv_obj_align(s_remain, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);

  s_state = lv_label_create(page);
  lv_label_set_text(s_state, "--");
  lv_obj_set_style_text_color(s_state, t->accent, 0);
  lv_obj_set_style_text_font(s_state, t->f_mono, 0);
  lv_obj_align(s_state, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  nowplaying_rec_t n = ds_get_nowplaying();
  uint32_t now = now_s();

  char buf[48];
  if (sv_status(buf, sizeof(buf), &n.hdr, now)) lv_label_set_text(s_src, buf);
  else if (n.has_device) {
    snprintf(buf, sizeof(buf), "> %s", n.device);
    lv_label_set_text(s_src, buf);
  } else lv_label_set_text(s_src, "> --");

  bool dim = sv_dim(n.hdr.state);
  bool ph  = sv_placeholder(n.hdr.state);

  if (ph || !n.has_device) {
    lv_label_set_text(s_title, ph ? "--" : "no active device");
    lv_label_set_text(s_artist, "--");
    lv_label_set_text(s_state, "--");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(s_elapsed, "-:--");
    lv_label_set_text(s_remain, "-:--");
    lv_obj_set_style_text_color(s_title, t->ink_dim, 0);
    return;
  }

  lv_label_set_text(s_title, n.title);
  lv_label_set_text(s_artist, n.artist);
  lv_obj_set_style_text_color(s_title, dim ? t->ink_dim : t->ink, 0);

  int32_t pos = 0;
  if (n.duration_ms > 0) {
    pos = (int32_t)((int64_t)n.progress_ms * 1000 / n.duration_ms);
    if (pos > 1000) pos = 1000;
  }
  lv_bar_set_value(s_bar, pos, LV_ANIM_OFF);

  char eb[12], rb[12];
  fmt_ms(eb, sizeof(eb), n.progress_ms);
  fmt_ms(rb, sizeof(rb), n.duration_ms);
  lv_label_set_text(s_elapsed, eb);
  lv_label_set_text(s_remain, rb);

  lv_label_set_text(s_state, n.playing ? "PLAYING" : "PAUSED");
  lv_obj_set_style_text_color(s_state, dim ? t->ink_dim : t->accent, 0);
}

extern const screen_view_t nowplaying_oscilloscope_view = { build, update };
