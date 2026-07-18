#include "ui/idle_glue.h"
#include "core/idle.h"
#include "core/nvs.h"
#include "core/datastore.h"
#include "core/records.h"
#include "ui/durations.h"
#include "ui/carousel.h"
#include "ui/pal_panel.h"
#include "hal/display.h"
#include <lvgl.h>
#include <string.h>

#define IDLE_DIM_RAW 24   // ~9% backlight while dimmed; on AMOLED this is clearly "asleep soon"

static uint32_t     s_dim_ms   = 0;
static uint32_t     s_sleep_ms = 0;
static idle_phase_t s_phase    = IDLE_ACTIVE;

// Wake-tap protection state.
static bool s_wake_tap = false;

static uint8_t clamp_idx(uint8_t i, uint8_t def) { return i < DURATION_COUNT ? i : def; }

void idle_apply_config_from_nvs(void) {
  s_dim_ms   = DURATIONS[clamp_idx(nvs_get_dim_idx(DURATION_DEFAULT_DIM), DURATION_DEFAULT_DIM)].ms;
  s_sleep_ms = DURATIONS[clamp_idx(nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP), DURATION_DEFAULT_SLEEP)].ms;
}

void idle_init(void) {
  idle_apply_config_from_nvs();
  s_phase = IDLE_ACTIVE;
}

bool idle_is_inactive(void) { return s_phase != IDLE_ACTIVE; }

// Keep-awake while PAL is watching a running agent: an agent can work for many minutes without
// any touch, and letting the panel sleep under it defeats the point of leaving the mascot open.
// Scoped to the open overlay so a background session never holds the whole device awake.
static bool pal_holds_awake(void) {
  if (!pal_panel_is_open()) return false;
  buddy_rec_t b = ds_get_buddy();
  for (uint8_t i = 0; i < b.session_count; i++)
    if (b.sessions[i].state == BST_WORKING) return true;
  return false;
}

void idle_service(void) {
  if (pal_holds_awake()) lv_disp_trig_activity(NULL);
  uint32_t inact = lv_disp_get_inactive_time(NULL);
  idle_phase_t p = idle_eval(inact, s_dim_ms, s_sleep_ms);
  if (p == s_phase) return;
  switch (p) {
    case IDLE_ACTIVE:
      // Wake the panel BEFORE restoring brightness: 0x51 written while the driver is in sleep-in is
      // not guaranteed to stick, which would leave the screen black after a wake tap.
      if (display_is_asleep()) {
        display_sleep(false);
        // GRAM content is not trusted across a sleep cycle, and any invalidation that happened while
        // we were asleep was flushed into a panel that wasn't scanning. Repaint everything once:
        // invalidation is area-based, so the full screen area also covers lv_layer_top overlays.
        lv_obj_invalidate(lv_scr_act());
      }
      display_brightness(nvs_get_brightness(204));
      break;
    case IDLE_DIM:
      display_brightness(IDLE_DIM_RAW);   // still awake: dim is the "about to sleep" warning
      break;
    case IDLE_SLEEP:
      display_brightness(0);   // blank first so the scan-out stop below is not visible as a flash
      display_sleep(true);
      break;
  }
  // #60: stop the carousel repaint tick while dim/asleep (no invalidations => no flushes => the panel
  // can sleep); resume + immediately refresh on wake. The brightness write above still runs first.
  carousel_set_tick_paused(p != IDLE_ACTIVE);
  s_phase = p;
}

void idle_note_press(bool was_inactive) { s_wake_tap = was_inactive; }

bool idle_take_wake_tap(void) {
  bool v = s_wake_tap;
  s_wake_tap = false;
  return v;
}

void buddy_wake_service(void) {
  buddy_rec_t b = ds_get_buddy();

  // Compute needs_user: a prompt is pending, or any session needs attention/response.
  bool needs_user = b.prompt.present;
  for (uint8_t i = 0; !needs_user && i < b.session_count; i++) {
    uint8_t st = b.sessions[i].state;
    needs_user = (st == BST_WAITING || st == BST_ATTENTION);
  }

  // Track previous state + prompt id to detect rising edges and new prompts while already needing.
  static bool s_prev_needs  = false;
  static char s_prev_prompt[BUDDY_ID_LEN];

  bool rising = (!s_prev_needs && needs_user) ||
                (needs_user && b.prompt.present &&
                 strncmp(s_prev_prompt, b.prompt.id, BUDDY_ID_LEN) != 0);

  if (rising && idle_is_inactive()) {
    lv_disp_trig_activity(NULL);
    carousel_goto_buddy();
  }

  s_prev_needs = needs_user;
  if (b.prompt.present) {
    strncpy(s_prev_prompt, b.prompt.id, BUDDY_ID_LEN - 1);
    s_prev_prompt[BUDDY_ID_LEN - 1] = '\0';
  } else {
    s_prev_prompt[0] = '\0';
  }

  // If the user left the PAL mascot overlay open when the device slept, a brand-new session
  // (any state, not just waiting/attention) should wake the screen so PAL can react -- but only
  // then: this must not wake the device onto the CLAUDE screen when PAL isn't the thing on top.
  static bool    s_ids_primed = false;
  static char    s_prev_ids[BUDDY_SESSIONS_MAX][BUDDY_SID_LEN];
  static uint8_t s_prev_id_count = 0;

  if (s_ids_primed && idle_is_inactive() && pal_panel_is_open()) {
    bool new_session = false;
    for (uint8_t i = 0; !new_session && i < b.session_count; i++) {
      bool seen = false;
      for (uint8_t j = 0; !seen && j < s_prev_id_count; j++)
        seen = strncmp(s_prev_ids[j], b.sessions[i].id, BUDDY_SID_LEN) == 0;
      new_session = !seen;
    }
    if (new_session) lv_disp_trig_activity(NULL);
  }

  s_prev_id_count = b.session_count;
  for (uint8_t i = 0; i < b.session_count; i++)
    memcpy(s_prev_ids[i], b.sessions[i].id, BUDDY_SID_LEN);
  s_ids_primed = true;
}
