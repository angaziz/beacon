#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// Aerospace HUD / Now Playing. "// NOW PLAYING" eyebrow + device (right), centered title +
// artist, a hairline progress bar (progress_ms/duration_ms), and a bottom transport row
// (elapsed | PLAYING/PAUSED | duration). has_device==false => "no active device" state.
// Transport controls are non-functional placeholders (P4).

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

static lv_obj_t *s_status;
static lv_obj_t *s_device;
static lv_obj_t *s_title, *s_artist;
static lv_obj_t *s_bar;
static lv_obj_t *s_elapsed, *s_state, *s_duration;
static lv_obj_t *s_nodev;

static void mmss(char* buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* title = lv_label_create(page);
  lv_obj_add_style(title, &S.slot, 0);
  lv_label_set_text(title, "// NOW PLAYING");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_obj_add_style(s_status, &S.slot, 0);
  lv_label_set_text(s_status, "");
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_device = lv_label_create(page);
  lv_obj_add_style(s_device, &S.slot, 0);
  lv_label_set_text(s_device, "");
  lv_obj_align(s_device, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET + 24);

  // Centered title + artist.
  s_title = lv_label_create(page);
  lv_obj_set_style_text_font(s_title, t->f_display, 0);
  lv_obj_set_style_text_color(s_title, t->ink, 0);
  lv_obj_set_width(s_title, SCREEN_W - 2 * SAFE_INSET);
  lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_label_set_text(s_title, "--");
  lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -24);

  s_artist = lv_label_create(page);
  lv_obj_add_style(s_artist, &S.slot, 0);
  lv_obj_set_width(s_artist, SCREEN_W - 2 * SAFE_INSET);
  lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
  lv_label_set_text(s_artist, "");
  lv_obj_align(s_artist, LV_ALIGN_CENTER, 0, 14);

  // Progress bar.
  s_bar = lv_bar_create(page);
  lv_obj_set_size(s_bar, SCREEN_W - 2 * SAFE_INSET, 4);
  lv_bar_set_range(s_bar, 0, 1000);
  lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bar, t->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, t->accent, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 36));

  // Transport row.
  s_elapsed = lv_label_create(page);
  lv_obj_add_style(s_elapsed, &S.slot, 0);
  lv_label_set_text(s_elapsed, "0:00");
  lv_obj_align(s_elapsed, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);

  s_state = lv_label_create(page);
  lv_obj_set_style_text_font(s_state, t->f_mono, 0);
  lv_obj_set_style_text_color(s_state, t->accent, 0);
  lv_label_set_text(s_state, "PAUSED");
  lv_obj_align(s_state, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  s_duration = lv_label_create(page);
  lv_obj_add_style(s_duration, &S.slot, 0);
  lv_label_set_text(s_duration, "0:00");
  lv_obj_align(s_duration, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);

  // No-device overlay (centered).
  s_nodev = lv_label_create(page);
  lv_obj_add_style(s_nodev, &S.slot, 0);
  lv_label_set_text(s_nodev, "no active device");
  lv_obj_align(s_nodev, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_nodev, LV_OBJ_FLAG_HIDDEN);
}

static void show_playback(bool show) {
  lv_obj_t* arr[] = { s_title, s_artist, s_bar, s_elapsed, s_state, s_duration, s_device };
  for (lv_obj_t* o : arr) {
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

static void update(void) {
  nowplaying_rec_t np = ds_get_nowplaying();
  uint32_t now = now_s();
  const beacon_theme_t* t = theme_active();

  char chip[24];
  if (sv_status(chip, sizeof(chip), &np.hdr, now)) {
    lv_label_set_text(s_status, chip);
    lv_obj_set_style_text_color(s_status, sv_severe(np.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "");
  }

  bool ph = sv_placeholder(np.hdr.state);
  bool dim = sv_dim(np.hdr.state);

  if (!np.has_device && !ph) {
    show_playback(false);
    lv_obj_clear_flag(s_nodev, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(s_nodev, LV_OBJ_FLAG_HIDDEN);
  show_playback(true);

  lv_label_set_text(s_title, ph || !np.title[0] ? "--" : np.title);
  lv_label_set_text(s_artist, ph ? "" : np.artist);
  lv_label_set_text(s_device, ph ? "" : np.device);
  lv_obj_set_style_text_color(s_title, dim ? t->ink_dim : t->ink, 0);

  uint32_t pos = 0;
  if (!ph && np.duration_ms > 0) {
    pos = (uint32_t)((uint64_t)np.progress_ms * 1000 / np.duration_ms);
    if (pos > 1000) pos = 1000;
  }
  lv_bar_set_value(s_bar, (int32_t)pos, LV_ANIM_OFF);

  char tb[12];
  mmss(tb, sizeof(tb), ph ? 0 : np.progress_ms);  lv_label_set_text(s_elapsed, tb);
  mmss(tb, sizeof(tb), ph ? 0 : np.duration_ms);  lv_label_set_text(s_duration, tb);

  if (ph) {
    lv_label_set_text(s_state, "...");
    lv_obj_set_style_text_color(s_state, t->ink_dim, 0);
  } else {
    lv_label_set_text(s_state, np.playing ? "PLAYING" : "PAUSED");
    lv_obj_set_style_text_color(s_state, np.playing ? t->accent : t->ink_dim, 0);
  }
}

extern const screen_view_t nowplaying_hud_view = { build, update };
