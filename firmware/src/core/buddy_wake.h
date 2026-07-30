#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "core/records.h"

// Auto-wake detector (issue #129): decides whether a buddy snapshot carries a *notable* hub event
// worth lighting a dim/asleep panel for. Pure — no LVGL, carousel, or clock (the caller passes
// monotonic uptime) — so [env:native] unit-tests it. ui/idle_glue owns the effects.
//
// Routine refreshes must NOT wake: the hub resends a byte-identical status frame every 30 s and
// bumps sessions[].ts on every activity tick, so this only ever reads prompt.present/prompt.id and
// each session's id + state. Everything else in buddy_rec_t is deliberately ignored.
#ifdef __cplusplus
extern "C" {
#endif

// Persisted as a byte in NVS (nvs_get/set_wake_mode). APPEND-ONLY: never reorder, or stored
// values shift meaning.
typedef enum {
  WAKE_OFF        = 0,   // auto-wake disabled entirely
  WAKE_PROMPTS    = 1,   // a tool-permission prompt only
  WAKE_ATTENTION  = 2,   // + a session entering waiting/attention
  WAKE_EVERYTHING = 3,   // + a new session or any session state change
} buddy_wake_mode_t;
#define BUDDY_WAKE_MODE_COUNT   4
#define BUDDY_WAKE_MODE_DEFAULT WAKE_EVERYTHING

// Wake-only events are debounced so a burst of session frames cannot pin the panel on. Needs-user
// events (prompt / waiting / attention) are never debounced -- a permission prompt must get through.
#define BUDDY_WAKE_COOLDOWN_S 10u

typedef enum {
  WAKE_ACT_NONE = 0,
  WAKE_ACT_ONLY,   // light the panel, stay on whatever screen the user left it on
  WAKE_ACT_SHOW,   // light the panel AND jump to the buddy screen (the user has to act)
} buddy_wake_action_t;

// Rolling baseline. Zero-init before the first call; buddy_wake_eval owns every field after that.
typedef struct {
  bool     prev_needs;
  char     prev_prompt[BUDDY_ID_LEN];
  char     prev_sid[BUDDY_SESSIONS_MAX][BUDDY_SID_LEN];
  uint8_t  prev_state[BUDDY_SESSIONS_MAX];
  uint8_t  prev_count;
  uint32_t last_wake_s;   // uptime of the last fired wake; 0 = never
} buddy_wake_state_t;

// Evaluate one snapshot. `now_s` is monotonic uptime; `inactive` is idle_is_inactive(). ALWAYS
// refreshes *st -- even when the action is suppressed -- so the baseline stays primed and the next
// real change reads as a true edge rather than a replay.
buddy_wake_action_t buddy_wake_eval(buddy_wake_state_t* st, const buddy_rec_t* b,
                                    uint8_t mode, uint32_t now_s, bool inactive);

#ifdef __cplusplus
}
#endif
