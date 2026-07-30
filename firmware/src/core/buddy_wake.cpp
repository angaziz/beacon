#include "core/buddy_wake.h"
#include <string.h>

static uint8_t session_n(const buddy_rec_t* b) {
  return b->session_count > BUDDY_SESSIONS_MAX ? BUDDY_SESSIONS_MAX : b->session_count;
}

// Baseline state recorded for `sid`, or -1 when the id is new to us.
static int prev_state_of(const buddy_wake_state_t* st, const char* sid) {
  for (uint8_t i = 0; i < st->prev_count; i++)
    if (strncmp(st->prev_sid[i], sid, BUDDY_SID_LEN) == 0) return (int)st->prev_state[i];
  return -1;
}

static void rebase(buddy_wake_state_t* st, const buddy_rec_t* b, bool needs) {
  st->prev_needs = needs;
  if (b->prompt.present) {
    strncpy(st->prev_prompt, b->prompt.id, BUDDY_ID_LEN - 1);
    st->prev_prompt[BUDDY_ID_LEN - 1] = '\0';
  } else {
    st->prev_prompt[0] = '\0';
  }
  uint8_t n = session_n(b);
  for (uint8_t i = 0; i < n; i++) {
    strncpy(st->prev_sid[i], b->sessions[i].id, BUDDY_SID_LEN - 1);
    st->prev_sid[i][BUDDY_SID_LEN - 1] = '\0';
    st->prev_state[i] = b->sessions[i].state;
  }
  st->prev_count = n;
}

buddy_wake_action_t buddy_wake_eval(buddy_wake_state_t* st, const buddy_rec_t* b,
                                    uint8_t mode, uint32_t now_s, bool inactive) {
  uint8_t n = session_n(b);

  // Needs-user: a prompt always counts; session waiting/attention only from WAKE_ATTENTION up.
  bool needs = b->prompt.present;
  if (!needs && mode >= WAKE_ATTENTION)
    for (uint8_t i = 0; !needs && i < n; i++) {
      uint8_t s = b->sessions[i].state;
      needs = (s == BST_WAITING || s == BST_ATTENTION);
    }

  bool needs_rising = (!st->prev_needs && needs) ||
                      (needs && b->prompt.present &&
                       strncmp(st->prev_prompt, b->prompt.id, BUDDY_ID_LEN) != 0);

  // Notable churn: an id we have never seen, or a known id whose state moved. Matched by id rather
  // than index because the hub re-sorts the list newest-first, so positions shift on their own.
  // A session vanishing is not notable -- nothing new needs looking at.
  bool session_notable = false;
  for (uint8_t i = 0; !session_notable && i < n; i++) {
    int was = prev_state_of(st, b->sessions[i].id);
    session_notable = (was < 0 || was != (int)b->sessions[i].state);
  }

  buddy_wake_action_t act = WAKE_ACT_NONE;
  if (mode != WAKE_OFF && inactive) {
    if (needs_rising) {
      act = WAKE_ACT_SHOW;                 // never debounced: the user has to act on this
    } else if (mode == WAKE_EVERYTHING && session_notable &&
               (st->last_wake_s == 0 ||    // 0 = never woken (cf. record_hdr_t.last_updated)
                now_s - st->last_wake_s >= BUDDY_WAKE_COOLDOWN_S)) {
      act = WAKE_ACT_ONLY;
    }
  }
  if (act != WAKE_ACT_NONE) st->last_wake_s = now_s;

  rebase(st, b, needs);   // unconditional: a suppressed event still moves the baseline forward
  return act;
}
