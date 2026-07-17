#include "ui/screens/screen_pal.h"
#include "ui/screens/pal_state.h"
#include "ui/theme.h"
#include "ui/carousel.h"
#include "ui/idle_glue.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <lvgl.h>
#include <string.h>

// Single shared view across all 7 themes (deliberate deviation from the SCREEN_MODULE_SIMPLE
// per-theme-view convention other screens use, e.g. screen_buddy.cpp): the mascot is fixed
// pixel art, not hand-authored layout, so only its background/border tint needs to vary by
// theme -- and that's already token-driven (theme_active()->bg/alert), same pattern gauge.cpp
// uses (switches on token fields, not theme_index()).

namespace {

// PAL is appended to carousel.cpp's MODULES[] after settings (home=0,finance=1,usage=2,
// buddy=3,settings=4,pal=5). Mirrors the existing magic-index comment for carousel_goto_buddy().
constexpr int PAL_SCREEN_INDEX = 5;

constexpr int32_t  ZOOM             = 3072;   // 20px source -> 240px on screen (256 = 1x, LVGL zoom units)
constexpr uint32_t IDLE_ROTATE_S    = 20;     // seconds between idle-pool animation switches
constexpr uint32_t BORDER_PULSE_MS  = 1100;   // one breathe cycle of the notification ring

lv_obj_t*   s_img          = nullptr;
lv_obj_t*   s_border       = nullptr;
lv_timer_t* s_frame_timer  = nullptr;
lv_anim_t   s_border_anim;
bool        s_border_anim_running = false;

pal_state_t   s_last_state   = (pal_state_t)-1;
pal_anim_id_t s_cur_anim_id  = PAL_ANIM_EXPRESSION_SLEEP;
uint16_t      s_cur_frame    = 0;
uint8_t       s_idle_rot_idx = 0;
uint32_t      s_idle_rotated_at = 0;

// One lv_img_dsc_t whose .data we repoint at the current animation's current frame -- each
// frame is a distinct const array in flash (pal_frames.c), no per-frame copy needed. Safe
// because LV_IMG_CACHE_DEF_SIZE is 0 (lv_conf.h): the decoder re-reads ((lv_img_dsc_t*)src)
// ->data fresh on every draw rather than caching a stale pointer from the first open.
lv_img_dsc_t s_img_dsc;

void set_frame(const pal_anim_t& a, uint16_t frame_idx) {
  s_img_dsc.data = a.frames[frame_idx];
  lv_obj_invalidate(s_img);
  lv_timer_set_period(s_frame_timer, a.holds_ms[frame_idx]);
}

void set_animation(pal_anim_id_t id) {
  s_cur_anim_id = id;
  s_cur_frame = 0;
  set_frame(PAL_ANIMS[id], 0);
}

void advance_frame(void) {
  const pal_anim_t& a = PAL_ANIMS[s_cur_anim_id];
  s_cur_frame = (uint16_t)((s_cur_frame + 1) % a.frame_count);
  set_frame(a, s_cur_frame);
}

// Fires every 100ms regardless (cheap int compares); only redraws while PAL is the visible
// screen and the panel isn't dim/asleep -- same rationale as the carousel's own #60 tick-pause:
// no invalidation while asleep => no QSPI flush => the panel can actually sleep.
void frame_timer_cb(lv_timer_t*) {
  if (carousel_current() != PAL_SCREEN_INDEX || idle_is_inactive()) return;
  advance_frame();
}

void border_opa_cb(void* obj, int32_t v) {
  lv_obj_set_style_border_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

void start_border_pulse(void) {
  if (s_border_anim_running) return;
  lv_obj_clear_flag(s_border, LV_OBJ_FLAG_HIDDEN);
  lv_anim_init(&s_border_anim);
  lv_anim_set_var(&s_border_anim, s_border);
  lv_anim_set_exec_cb(&s_border_anim, border_opa_cb);
  lv_anim_set_values(&s_border_anim, 100, 255);
  lv_anim_set_time(&s_border_anim, BORDER_PULSE_MS);
  lv_anim_set_playback_time(&s_border_anim, BORDER_PULSE_MS);
  lv_anim_set_repeat_count(&s_border_anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_border_anim, lv_anim_path_ease_in_out);
  lv_anim_start(&s_border_anim);
  s_border_anim_running = true;
}

void stop_border_pulse(void) {
  if (!s_border_anim_running) return;
  lv_anim_del(s_border, border_opa_cb);
  lv_obj_add_flag(s_border, LV_OBJ_FLAG_HIDDEN);
  s_border_anim_running = false;
}

lv_obj_t* build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_set_style_bg_color(page, t->bg, 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

  // Pulsing notification ring: full-bleed, border-only (transparent fill), hidden except in
  // PAL_STATE_NOTIFY. 70px radius per design; deliberately NOT SAFE_INSET-bound since it's a
  // screen-edge alert, not content.
  s_border = lv_obj_create(page);
  lv_obj_remove_style_all(s_border);
  lv_obj_set_size(s_border, SCREEN_W, SCREEN_H);
  lv_obj_center(s_border);
  lv_obj_set_style_bg_opa(s_border, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(s_border, 70, 0);
  lv_obj_set_style_border_width(s_border, 7, 0);
  lv_obj_set_style_border_color(s_border, t->alert, 0);
  lv_obj_set_style_border_opa(s_border, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(s_border, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_border, LV_OBJ_FLAG_HIDDEN);
  s_border_anim_running = false;

  memset(&s_img_dsc, 0, sizeof(s_img_dsc));
  s_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  s_img_dsc.header.w  = PAL_FRAME_W;
  s_img_dsc.header.h  = PAL_FRAME_H;
  s_img_dsc.data_size = PAL_FRAME_BYTES;
  s_img_dsc.data      = PAL_ANIMS[PAL_ANIM_EXPRESSION_SLEEP].frames[0];   // set_animation() below repoints this

  s_img = lv_img_create(page);
  lv_img_set_src(s_img, &s_img_dsc);
  lv_img_set_zoom(s_img, ZOOM);
  lv_img_set_antialias(s_img, false);   // nearest-neighbor: keep pixel-art edges crisp at this zoom
  lv_obj_center(s_img);

  // Must exist before set_animation() below -- it calls lv_timer_set_period() on this.
  if (!s_frame_timer) s_frame_timer = lv_timer_create(frame_timer_cb, 100, NULL);

  s_last_state = (pal_state_t)-1;   // force update() to (re)pick + animate on the next tick
  set_animation(PAL_ANIM_EXPRESSION_SLEEP);

  return page;
}

void update(void) {
  buddy_rec_t b = ds_get_buddy();
  pal_state_t st = pal_pick_state(&b);
  uint32_t now = uptime_s();

  if (st != s_last_state) {
    s_last_state = st;
    s_idle_rot_idx = 0;
    s_idle_rotated_at = now;
    set_animation(pal_anim_for_state(st, s_idle_rot_idx));
    if (st == PAL_STATE_NOTIFY) start_border_pulse(); else stop_border_pulse();
  } else if (st == PAL_STATE_IDLE && (now - s_idle_rotated_at) >= IDLE_ROTATE_S) {
    s_idle_rot_idx++;
    s_idle_rotated_at = now;
    set_animation(pal_anim_for_state(st, s_idle_rot_idx));
  }
}

}  // namespace

const screen_module_t pal_module = {"PAL", build, update};
