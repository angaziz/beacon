#include "ui/idle_glue.h"
#include "core/idle.h"
#include "core/nvs.h"
#include "ui/durations.h"
#include "hal/display.h"
#include <lvgl.h>

#define IDLE_DIM_RAW 24   // ~9% backlight while dimmed; on AMOLED this is clearly "asleep soon"

static uint32_t     s_dim_ms   = 0;
static uint32_t     s_sleep_ms = 0;
static idle_phase_t s_phase    = IDLE_ACTIVE;

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

void idle_service(void) {
  uint32_t inact = lv_disp_get_inactive_time(NULL);
  idle_phase_t p = idle_eval(inact, s_dim_ms, s_sleep_ms);
  if (p == s_phase) return;
  switch (p) {
    case IDLE_ACTIVE: display_brightness(nvs_get_brightness(204)); break;
    case IDLE_DIM:    display_brightness(IDLE_DIM_RAW);            break;
    case IDLE_SLEEP:  display_brightness(0);                       break;
  }
  s_phase = p;
}
