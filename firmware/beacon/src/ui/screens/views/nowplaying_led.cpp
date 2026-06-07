#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// LED Matrix / NOW: title (lit), artist (dim), device in header, progress cell-bar, PLAYING/PAUSED.
// has_device==false => "no active device".

static lv_obj_t *s_device, *s_title, *s_artist;
static lv_obj_t *s_bar, *s_elapsed, *s_state, *s_total;

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

static void fmt_ms(char* buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;

  lv_obj_t* eb = lv_label_create(page);
  lv_label_set_text(eb, "BEACON / NOW PLAYING");
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->accent, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_device = lv_label_create(page);
  lv_label_set_text(s_device, "");
  lv_obj_set_style_text_font(s_device, t->f_mono, 0);
  lv_obj_set_style_text_color(s_device, t->ink_dim, 0);
  lv_obj_align(s_device, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_title = lv_label_create(page);
  lv_label_set_text(s_title, "--");
  lv_obj_set_width(s_title, SCREEN_W - 2 * SAFE_INSET);
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_title, t->f_display, 0);
  lv_obj_set_style_text_color(s_title, t->accent, 0);
  lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -20);

  s_artist = lv_label_create(page);
  lv_label_set_text(s_artist, "--");
  lv_obj_set_width(s_artist, SCREEN_W - 2 * SAFE_INSET);
  lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_artist, t->f_mono, 0);
  lv_obj_set_style_text_color(s_artist, t->ink_dim, 0);
  lv_obj_align_to(s_artist, s_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, SPACE_S);

  s_bar = lv_bar_create(page);
  lv_obj_set_size(s_bar, SCREEN_W - 2 * SAFE_INSET, 8);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET - 28);
  lv_bar_set_range(s_bar, 0, 1000);
  lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bar, t->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, t->accent, LV_PART_INDICATOR);

  lv_obj_t* foot = lv_obj_create(page);
  lv_obj_remove_style_all(foot);
  lv_obj_set_size(foot, SCREEN_W - 2 * SAFE_INSET, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  s_elapsed = lv_label_create(foot);
  lv_label_set_text(s_elapsed, "0:00");
  lv_obj_set_style_text_font(s_elapsed, t->f_mono, 0);
  lv_obj_set_style_text_color(s_elapsed, t->ink_dim, 0);

  s_state = lv_label_create(foot);
  lv_label_set_text(s_state, "PAUSED");
  lv_obj_set_style_text_font(s_state, t->f_mono, 0);
  lv_obj_set_style_text_color(s_state, t->accent, 0);

  s_total = lv_label_create(foot);
  lv_label_set_text(s_total, "0:00");
  lv_obj_set_style_text_font(s_total, t->f_mono, 0);
  lv_obj_set_style_text_color(s_total, t->ink_dim, 0);
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;
  nowplaying_rec_t np = ds_get_nowplaying();
  uint32_t now = now_s();

  char st[24];
  if (sv_status(st, sizeof(st), &np.hdr, now)) {
    lv_label_set_text(s_device, st);
    lv_obj_set_style_text_color(s_device, sv_severe(np.hdr.state) ? t->down : t->ink_dim, 0);
  } else if (!np.has_device) {
    lv_label_set_text(s_device, "NO DEVICE");
    lv_obj_set_style_text_color(s_device, t->ink_dim, 0);
  } else {
    lv_label_set_text(s_device, np.device[0] ? np.device : "--");
    lv_obj_set_style_text_color(s_device, t->ink_dim, 0);
  }

  if (!np.has_device || sv_placeholder(np.hdr.state)) {
    lv_label_set_text(s_title, "no active device");
    lv_label_set_text(s_artist, "--");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(s_elapsed, "0:00");
    lv_label_set_text(s_total, "0:00");
    lv_label_set_text(s_state, "--");
    return;
  }

  lv_color_t vc = sv_dim(np.hdr.state) ? t->ink_dim : t->accent;
  lv_label_set_text(s_title, np.title[0] ? np.title : "--");
  lv_obj_set_style_text_color(s_title, vc, 0);
  lv_label_set_text(s_artist, np.artist[0] ? np.artist : "--");

  int32_t v = 0;
  if (np.duration_ms > 0) {
    uint64_t num = (uint64_t)np.progress_ms * 1000u;
    v = (int32_t)(num / np.duration_ms);
    if (v > 1000) v = 1000;
  }
  lv_bar_set_value(s_bar, v, LV_ANIM_OFF);

  char buf[16];
  fmt_ms(buf, sizeof(buf), np.progress_ms);
  lv_label_set_text(s_elapsed, buf);
  fmt_ms(buf, sizeof(buf), np.duration_ms);
  lv_label_set_text(s_total, buf);
  lv_label_set_text(s_state, np.playing ? "PLAYING" : "PAUSED");
}

extern const screen_view_t nowplaying_led_view = { build, update };
