#include "ui/idle_glue.h"
#include "core/idle.h"
#include "core/nvs.h"
#include "core/datastore.h"
#include "core/records.h"
#include "core/buddy_wake.h"
#include "ui/durations.h"
#include "ui/carousel.h"
#include "ui/screen.h"
#include "hal/display.h"
#include <lvgl.h>

#define IDLE_DIM_RAW 24   // ~9% backlight while dimmed; on AMOLED this is clearly "asleep soon"

static uint32_t     s_dim_ms   = 0;
static uint32_t     s_sleep_ms = 0;
static idle_phase_t s_phase    = IDLE_ACTIVE;
static uint8_t      s_wake_mode = BUDDY_WAKE_MODE_DEFAULT;   // cached: buddy_wake_service is hot

// Wake-tap protection state.
static bool s_wake_tap = false;

static uint8_t clamp_idx(uint8_t i, uint8_t def) { return i < DURATION_COUNT ? i : def; }

void idle_apply_config_from_nvs(void) {
  s_dim_ms   = DURATIONS[clamp_idx(nvs_get_dim_idx(DURATION_DEFAULT_DIM), DURATION_DEFAULT_DIM)].ms;
  s_sleep_ms = DURATIONS[clamp_idx(nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP), DURATION_DEFAULT_SLEEP)].ms;
  uint8_t w  = nvs_get_wake_mode(BUDDY_WAKE_MODE_DEFAULT);
  s_wake_mode = w < BUDDY_WAKE_MODE_COUNT ? w : (uint8_t)BUDDY_WAKE_MODE_DEFAULT;
}

void idle_init(void) {
  idle_apply_config_from_nvs();
  s_phase = IDLE_ACTIVE;
}

bool idle_is_inactive(void) { return s_phase != IDLE_ACTIVE; }

void idle_service(void) {
  uint32_t inact = lv_disp_get_inactive_time(NULL);
  idle_phase_t p = idle_eval(inact, s_dim_ms, s_sleep_ms);
  if (p == s_phase) return;
  switch (p) {
    case IDLE_ACTIVE: display_brightness(nvs_get_brightness(204)); break;
    case IDLE_DIM:    display_brightness(IDLE_DIM_RAW);            break;
    case IDLE_SLEEP:  display_brightness(0);                       break;
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
  static buddy_wake_state_t s_wake;
  buddy_rec_t b = ds_get_buddy();

  switch (buddy_wake_eval(&s_wake, &b, s_wake_mode, uptime_s(), idle_is_inactive())) {
    case WAKE_ACT_SHOW:
      lv_disp_trig_activity(NULL);
      carousel_goto_buddy();          // the user has to act: put the prompt in front of them
      break;
    case WAKE_ACT_ONLY:
      lv_disp_trig_activity(NULL);    // notable, but not actionable: leave the screen where it was
      break;
    case WAKE_ACT_NONE:
      break;
  }
}
