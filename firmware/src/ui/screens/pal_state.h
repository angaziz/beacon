#pragma once
#include <stdint.h>
#include "core/records.h"
#include "ui/screens/pal_frames.h"

// Which animation category the PAL mascot should show. Pure function of buddy_rec_t so
// it's unit-testable without LVGL/Arduino (mirrors screen_common.h's buddy_queue_badge).
typedef enum {
  PAL_STATE_SLEEP,    // no hub link, or hub link up but no sessions at all
  PAL_STATE_NOTIFY,   // a tool-permission prompt is pending -- needs the user right now
  PAL_STATE_ACTIVE,   // a session is actively working
  PAL_STATE_IDLE,     // has agent, nothing pending or working
} pal_state_t;

// Precedence (high to low): a pending prompt always wins -- it demands attention even if
// some other session happens to be working. Then hub-offline/no-sessions is "asleep".
// Then any working session is "active". Everything else is idle.
static inline pal_state_t pal_pick_state(const buddy_rec_t* b) {
  if (b->prompt.present) return PAL_STATE_NOTIFY;
  if (b->hdr.state == ST_HUB_OFFLINE || b->session_count == 0) return PAL_STATE_SLEEP;
  for (uint8_t i = 0; i < b->session_count; i++)
    if (b->sessions[i].state == BST_WORKING) return PAL_STATE_ACTIVE;
  return PAL_STATE_IDLE;
}

// Idle-state rotation pool, in rotation order (~8s/animation, see screen_pal.cpp).
// expression_surprise and work_think are deliberately excluded here -- they're reserved
// as one-shot tap reactions (idle and sleep respectively; see screen_pal.cpp's click handler).
static const pal_anim_id_t PAL_IDLE_POOL[] = {
  PAL_ANIM_IDLE_BREATHE,     PAL_ANIM_IDLE_BLINK,
  PAL_ANIM_IDLE_LOOK_AROUND, PAL_ANIM_EXPRESSION_WINK,  PAL_ANIM_DANCE_SWAY,
};
#define PAL_IDLE_POOL_LEN (sizeof(PAL_IDLE_POOL) / sizeof(PAL_IDLE_POOL[0]))

// Maps a mascot state (+ current idle-rotation slot) to the animation to play.
static inline pal_anim_id_t pal_anim_for_state(pal_state_t s, uint8_t idle_rot_idx) {
  switch (s) {
    case PAL_STATE_SLEEP:  return PAL_ANIM_EXPRESSION_SLEEP;
    case PAL_STATE_NOTIFY: return PAL_ANIM_DANCE_BOUNCE;
    case PAL_STATE_ACTIVE: return PAL_ANIM_WORK_CODING;
    default:                return PAL_IDLE_POOL[idle_rot_idx % PAL_IDLE_POOL_LEN];
  }
}
