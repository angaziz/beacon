// Calm Futurism NOW (now-playing) view. Sparse white-on-black. Top: [dot] now + device slot.
// Centered big Doto title + dim artist. A hairline progress bar near the lower band with elapsed /
// total times and a PLAYING/PAUSED marker. has_device==false => "no active device".
// Controls are non-functional placeholders (P4). Background + chrome drawn by the carousel.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>
static void update(void);


static lv_obj_t *s_device, *s_status;
static lv_obj_t *s_title, *s_artist;
static lv_obj_t *s_bar, *s_elapsed, *s_total, *s_state;

static void ms_to_clock(char* buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dot = lv_obj_create(page);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, t->accent, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 4, SAFE_INSET + 12);

  lv_obj_t* brand = lv_label_create(page);
  lv_label_set_text(brand, "now");
  lv_obj_set_style_text_font(brand, t->f_body, 0);
  lv_obj_set_style_text_color(brand, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(brand, 3, 0);
  lv_obj_align(brand, LV_ALIGN_TOP_LEFT, SAFE_INSET + 20, SAFE_INSET + 8);

  s_device = lv_label_create(page);
  lv_label_set_text(s_device, "--");
  lv_obj_set_style_text_font(s_device, t->f_body, 0);
  lv_obj_set_style_text_color(s_device, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_device, 2, 0);
  lv_obj_align(s_device, LV_ALIGN_TOP_RIGHT, -(SAFE_INSET + 4), SAFE_INSET + 8);

  s_status = lv_label_create(page);
  lv_obj_set_style_text_font(s_status, t->f_body, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_status, 2, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -(SAFE_INSET + 4), SAFE_INSET + 26);
  lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

  s_title = lv_label_create(page);
  lv_label_set_text(s_title, "--");
  lv_obj_set_style_text_font(s_title, t->f_display, 0);
  lv_obj_set_style_text_color(s_title, t->ink, 0);
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_title, SCREEN_W - 2 * SAFE_INSET);
  lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -24);

  s_artist = lv_label_create(page);
  lv_label_set_text(s_artist, "--");
  lv_obj_set_style_text_font(s_artist, t->f_body, 0);
  lv_obj_set_style_text_color(s_artist, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_artist, 2, 0);
  lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_artist, SCREEN_W - 2 * SAFE_INSET);
  lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_artist, LV_ALIGN_CENTER, 0, 18);

  // Hairline progress bar. lv_bar parts are OFF by default => force COVER + colors.
  s_bar = lv_bar_create(page);
  lv_obj_remove_style_all(s_bar);
  lv_obj_set_size(s_bar, SCREEN_W - 2 * SAFE_INSET, 2);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 28));
  lv_bar_set_range(s_bar, 0, 1000);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, t->line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bar, t->ink, LV_PART_INDICATOR);
  lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

  s_elapsed = lv_label_create(page);
  lv_label_set_text(s_elapsed, "0:00");
  lv_obj_set_style_text_font(s_elapsed, t->f_body, 0);
  lv_obj_set_style_text_color(s_elapsed, t->ink_dim, 0);
  lv_obj_align(s_elapsed, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET + 4, -SAFE_INSET);

  s_total = lv_label_create(page);
  lv_label_set_text(s_total, "0:00");
  lv_obj_set_style_text_font(s_total, t->f_body, 0);
  lv_obj_set_style_text_color(s_total, t->ink_dim, 0);
  lv_obj_align(s_total, LV_ALIGN_BOTTOM_RIGHT, -(SAFE_INSET + 4), -SAFE_INSET);

  s_state = lv_label_create(page);
  lv_label_set_text(s_state, "");
  lv_obj_set_style_text_font(s_state, t->f_body, 0);
  lv_obj_set_style_text_color(s_state, t->accent, 0);
  lv_obj_set_style_text_letter_space(s_state, 2, 0);
  lv_obj_align(s_state, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  nowplaying_rec_t n = ds_get_nowplaying();
  uint32_t now = now_s();

  bool ph = sv_placeholder(n.hdr.state);
  bool dim = sv_dim(n.hdr.state);

  char sbuf[24];
  if (sv_status(sbuf, sizeof(sbuf), &n.hdr, now)) {
    lv_label_set_text(s_status, sbuf);
    lv_obj_set_style_text_color(s_status, sv_severe(n.hdr.state) ? t->down : t->ink_dim, 0);
    lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
  }

  if (!ph && !n.has_device) {
    lv_label_set_text(s_device, "no device");
    lv_label_set_text(s_title, "no active device");
    lv_label_set_text(s_artist, "");
    lv_obj_set_style_text_color(s_title, t->ink_dim, 0);
    lv_label_set_text(s_state, "");
    lv_label_set_text(s_elapsed, "0:00");
    lv_label_set_text(s_total, "0:00");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    return;
  }

  if (ph) {
    lv_label_set_text(s_title, "--");
    lv_label_set_text(s_artist, "--");
    lv_label_set_text(s_device, "--");
    lv_label_set_text(s_state, "");
    lv_label_set_text(s_elapsed, "0:00");
    lv_label_set_text(s_total, "0:00");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    return;
  }

  lv_label_set_text(s_title, n.title[0] ? n.title : "--");
  lv_label_set_text(s_artist, n.artist);
  lv_label_set_text(s_device, n.device[0] ? n.device : "--");
  lv_obj_set_style_text_color(s_title, dim ? t->ink_dim : t->ink, 0);

  int32_t pos = 0;
  if (n.duration_ms > 0) {
    pos = (int32_t)((uint64_t)n.progress_ms * 1000 / n.duration_ms);
    if (pos > 1000) pos = 1000;
  }
  lv_bar_set_value(s_bar, pos, LV_ANIM_OFF);

  char tb[12];
  ms_to_clock(tb, sizeof(tb), n.progress_ms); lv_label_set_text(s_elapsed, tb);
  ms_to_clock(tb, sizeof(tb), n.duration_ms); lv_label_set_text(s_total, tb);

  lv_label_set_text(s_state, n.playing ? "playing" : "paused");
  lv_obj_set_style_text_color(s_state, n.playing ? t->accent : t->ink_dim, 0);
}

extern const screen_view_t nowplaying_calm_view = { build, update };
