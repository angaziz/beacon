#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "ui/theme.h"
#include "core/datastore.h"

static lv_obj_t *s_slot, *s_title, *s_artist, *s_device, *s_prog, *s_state, *s_none;

static void build(lv_obj_t* page) {
  s_slot = build_header(page, "NOW");
  s_title = lv_label_create(page); lv_obj_add_style(s_title, &S.display, 0);
  lv_obj_set_width(s_title, SCREEN_W - 2*SAFE_INSET); lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_align(s_title, LV_ALIGN_LEFT_MID, SAFE_INSET, -40);
  s_artist = lv_label_create(page); lv_obj_add_style(s_artist, &S.body, 0); lv_obj_add_style(s_artist, &S.dim, 0);
  lv_obj_align(s_artist, LV_ALIGN_LEFT_MID, SAFE_INSET, 0);
  s_prog = lv_bar_create(page); lv_obj_set_size(s_prog, SCREEN_W - 2*SAFE_INSET, 4);
  lv_obj_align(s_prog, LV_ALIGN_LEFT_MID, SAFE_INSET, 40); lv_bar_set_range(s_prog, 0, 1000);
  s_state = lv_label_create(page); lv_obj_add_style(s_state, &S.slot, 0); lv_obj_align(s_state, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);
  s_device = lv_label_create(page); lv_obj_add_style(s_device, &S.slot, 0);
  lv_obj_set_width(s_device, 200); lv_obj_set_style_text_align(s_device, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(s_device, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
  s_none = lv_label_create(page); lv_obj_add_style(s_none, &S.slot, 0); lv_label_set_text(s_none, "no active device"); lv_obj_center(s_none);
}

static void show_player(bool on) {
  lv_obj_t* p[] = {s_title, s_artist, s_prog, s_state, s_device};
  for (size_t i = 0; i < sizeof(p)/sizeof(p[0]); i++) {
    if (on) lv_obj_clear_flag(p[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(p[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (on) lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_none, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  nowplaying_rec_t n = ds_get_nowplaying(); uint32_t now = now_s();
  slot_set(s_slot, "NOW", &n.hdr, now);
  if (!n.has_device || sv_placeholder(n.hdr.state)) { show_player(false); return; }
  show_player(true);
  const beacon_theme_t* th = theme_active();
  lv_obj_set_style_bg_opa(s_prog, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_prog, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_prog, th->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_prog, th->accent, LV_PART_INDICATOR);
  lv_label_set_text(s_title, n.title);
  lv_label_set_text(s_artist, n.artist);
  lv_label_set_text(s_device, n.device);
  lv_label_set_text(s_state, n.playing ? "PLAYING" : "PAUSED");
  int32_t pos = (n.duration_ms > 0) ? (int32_t)((uint64_t)n.progress_ms * 1000 / n.duration_ms) : 0;
  lv_bar_set_value(s_prog, pos, LV_ANIM_OFF);
}

extern const screen_view_t nowplaying_editorial_view = { build, update };
