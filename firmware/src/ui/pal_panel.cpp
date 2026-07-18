#include "ui/pal_panel.h"
#include "ui/screens/pal_state.h"
#include "ui/theme.h"
#include "ui/styles.h"
#include "ui/carousel.h"
#include "ui/idle_glue.h"
#include "ui/screen.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <lvgl.h>
#include <string.h>

// Full-screen modal overlay (see wifi_panel.cpp for the established open/close/freeze-swipe
// pattern this follows). Two distinct touch interactions on the same full-screen root, kept from
// conflicting by LVGL's own click-vs-gesture arbitration (a click has ~0 drift; a gesture needs
// gesture_limit=50px of deliberate motion, lv_conf default) plus an explicit
// lv_indev_wait_release() whenever a swipe is recognized, so the swipe that opened/closes this
// panel can never also fire a stray tap-reaction or a click on whatever was underneath the finger
// on the CLAUDE screen (e.g. an approve/deny button):
//   - tap                    -> one-shot reaction animation (reaction_for_state), see click_cb.
//   - vertical swipe (up/dn) -> close (screen_buddy.cpp wires the matching open gesture).

namespace {

constexpr int32_t  ZOOM             = 3072;   // 20px source -> 240px on screen (256 = 1x, LVGL zoom units)
constexpr uint32_t IDLE_ROTATE_S    = 8;      // seconds between idle-pool animation switches
constexpr uint32_t BORDER_PULSE_MS  = 1100;   // one breathe cycle of the notification ring

lv_obj_t*   s_root         = nullptr;   // full-screen modal on lv_layer_top (see wifi_panel.cpp)
lv_obj_t*   s_img          = nullptr;
lv_obj_t*   s_border       = nullptr;
lv_obj_t*   s_label        = nullptr;
lv_timer_t* s_frame_timer  = nullptr;
lv_anim_t   s_border_anim;
bool        s_border_anim_running = false;
bool        s_paused              = false;   // held across a screen rotation (ui/rotation.cpp)
bool        s_border_parked       = false;   // pulse was stopped by dim/sleep; restart it on wake

pal_state_t   s_last_state      = (pal_state_t)-1;
pal_anim_id_t s_cur_anim_id     = PAL_ANIM_EXPRESSION_SLEEP;
uint16_t      s_cur_frame       = 0;
uint8_t       s_idle_rot_idx    = 0;
uint32_t      s_idle_rotated_at = 0;
bool          s_one_shot        = false;   // playing a tap-triggered reaction, not the state animation

const char* status_text(pal_state_t s) {
  switch (s) {
    case PAL_STATE_SLEEP:  return "SLEEPING";
    case PAL_STATE_NOTIFY: return "NEEDS YOU";
    case PAL_STATE_ACTIVE: return "THINKING";
    default:                return "WAITING";   // PAL_STATE_IDLE
  }
}

// Tap reaction per base state -- a little easter egg, distinct from the looped state animation.
// Returns false for states with no defined reaction (NOTIFY: the pulsing border already has the
// user's attention, no need to interrupt dance_bounce).
bool reaction_for_state(pal_state_t s, pal_anim_id_t* out) {
  switch (s) {
    case PAL_STATE_IDLE:   *out = PAL_ANIM_EXPRESSION_SURPRISE; return true;
    case PAL_STATE_ACTIVE: *out = PAL_ANIM_DANCE_DJMIX;         return true;
    case PAL_STATE_SLEEP:  *out = PAL_ANIM_WORK_THINK;          return true;
    default:                return false;                        // PAL_STATE_NOTIFY
  }
}

// One lv_img_dsc_t whose .data we repoint at the current animation's current frame -- each frame
// is a distinct const array in flash (pal_frames.c), no per-frame copy needed. Safe because
// LV_IMG_CACHE_DEF_SIZE is 0 (lv_conf.h): the decoder re-reads ((lv_img_dsc_t*)src)->data fresh
// on every draw rather than caching a stale pointer from the first open.
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
  uint16_t next = (uint16_t)((s_cur_frame + 1) % a.frame_count);
  if (s_one_shot && next == 0) {
    // The one-shot reaction played through exactly once -- hand back to whatever the mascot's
    // actual state animation is now (may have changed while the reaction played).
    s_one_shot = false;
    set_animation(pal_anim_for_state(s_last_state, s_idle_rot_idx));
    return;
  }
  s_cur_frame = next;
  set_frame(a, s_cur_frame);
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

// Poll buddy state and switch animation/border/label on a real transition, or rotate the idle
// pool every IDLE_ROTATE_S. Folded into the same timer as advance_frame() (frame_timer_cb below)
// instead of a separate carousel-tick call -- the panel isn't a carousel module anymore, so
// nothing else drives it.
void check_state(void) {
  buddy_rec_t b = ds_get_buddy();
  pal_state_t st = pal_pick_state(&b);
  uint32_t now = uptime_s();

  if (st != s_last_state) {
    s_last_state = st;
    s_idle_rot_idx = 0;
    s_idle_rotated_at = now;
    s_one_shot = false;   // a real state change pre-empts any in-flight tap reaction
    set_animation(pal_anim_for_state(st, s_idle_rot_idx));
    lv_label_set_text(s_label, status_text(st));
    if (st == PAL_STATE_NOTIFY) start_border_pulse(); else stop_border_pulse();
  } else if (!s_one_shot && st == PAL_STATE_IDLE && (now - s_idle_rotated_at) >= IDLE_ROTATE_S) {
    s_idle_rot_idx++;
    s_idle_rotated_at = now;
    set_animation(pal_anim_for_state(st, s_idle_rot_idx));
  }
}

// Re-armed each frame to that frame's hold_ms. No-ops while dimmed/asleep so the panel can
// actually sleep (same #60 rationale as the rest of the app: no invalidation => no QSPI flush).
//
// Returning early is enough for the sprite (its only invalidation source is set_frame(), below us)
// but NOT for the notification ring: start_border_pulse() runs as an lv_anim, which LVGL drives
// from its own timer handler and which therefore never sees this gate. Left alone it repaints a
// full-screen 466x466 border forever -- a whole-frame QSPI flush per step, all night, at
// brightness 0 where nothing is even visible. So park it on the way into dim/sleep and re-arm it
// on wake. check_state() alone can't do the re-arm: s_last_state is still NOTIFY across the sleep,
// so it sees no transition and never calls start_border_pulse() again.
void frame_timer_cb(lv_timer_t*) {
  if (s_paused) return;
  if (idle_is_inactive()) {
    if (s_border_anim_running) { stop_border_pulse(); s_border_parked = true; }
    return;
  }
  if (s_border_parked) {
    s_border_parked = false;
    if (s_last_state == PAL_STATE_NOTIFY) start_border_pulse();
  }
  check_state();
  advance_frame();
}

void click_cb(lv_event_t*) {
  pal_anim_id_t reaction;
  if (!reaction_for_state(s_last_state, &reaction)) return;
  set_animation(reaction);
  s_one_shot = true;
}

void gesture_cb(lv_event_t*) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) return;
  // This press was a swipe, not a tap: stop it from also firing CLICKED (click_cb, above) once
  // released. lv_indev_wait_release() is the LVGL-documented way to do that.
  lv_indev_wait_release(lv_indev_get_act());
  pal_panel_close();
}

}  // namespace

void pal_panel_open(void) {
  if (s_root) return;   // already open (idempotent)
  const beacon_theme_t* t = theme_active();
  carousel_set_swipe_enabled(false);

  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
  lv_obj_center(s_root);
  lv_obj_set_style_bg_color(s_root, t->bg, 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);   // land LV_EVENT_GESTURE here, don't bubble away
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);          // capture taps so none leak to the carousel
  lv_obj_add_event_cb(s_root, click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, NULL);

  // Pulsing notification ring: full-bleed, border-only (transparent fill), hidden except in
  // PAL_STATE_NOTIFY. 70px radius per design.
  s_border = lv_obj_create(s_root);
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
  s_border_parked       = false;

  memset(&s_img_dsc, 0, sizeof(s_img_dsc));
  s_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  s_img_dsc.header.w  = PAL_FRAME_W;
  s_img_dsc.header.h  = PAL_FRAME_H;
  s_img_dsc.data_size = PAL_FRAME_BYTES;
  s_img_dsc.data      = PAL_ANIMS[PAL_ANIM_EXPRESSION_SLEEP].frames[0];   // set_animation() below repoints this

  s_img = lv_img_create(s_root);
  lv_img_set_src(s_img, &s_img_dsc);
  lv_img_set_zoom(s_img, ZOOM);
  lv_img_set_antialias(s_img, false);   // nearest-neighbor: keep pixel-art edges crisp at this zoom
  lv_obj_center(s_img);
  lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);   // s_root owns the tap target, not the sprite

  s_label = lv_label_create(s_root);
  lv_obj_add_style(s_label, &S.slot, 0);
  lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  // Must exist BEFORE set_animation() below: set_frame() calls lv_timer_set_period() on it, so a
  // null s_frame_timer here is a null-pointer write -> StoreProhibited panic -> reboot (the exact
  // bug the original carousel-screen version hit; do not reorder these two).
  s_frame_timer = lv_timer_create(frame_timer_cb, 100, NULL);

  s_last_state = (pal_state_t)-1;   // force check_state() to (re)pick + animate right away
  s_one_shot = false;
  set_animation(PAL_ANIM_EXPRESSION_SLEEP);
  lv_label_set_text(s_label, status_text(PAL_STATE_SLEEP));

  check_state();   // pick up the real state immediately instead of waiting for the first tick
}

void pal_panel_close(void) {
  if (!s_root) return;
  stop_border_pulse();
  s_border_parked = false;
  if (s_frame_timer) { lv_timer_del(s_frame_timer); s_frame_timer = nullptr; }
  lv_obj_del(s_root);   // cascades to s_border/s_img/s_label
  s_root = s_img = s_border = s_label = nullptr;
  carousel_set_swipe_enabled(true);   // unconditional restore -- never leave the carousel frozen
}

bool pal_panel_is_open(void) { return s_root != nullptr; }

// Freeze the mascot without tearing down its state: the frame timer keeps running but no-ops, so the
// current frame stays on screen and animation resumes from where it left off. Used by ui/rotation.cpp
// so the mascot does not step mid-repaint while the screen turns.
void pal_panel_set_paused(bool paused) { s_paused = paused; }
