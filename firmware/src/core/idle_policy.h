#pragma once
#include <stdint.h>
// Pure idle-power policy (LVGL-free, host-tested). Given how long the UI has been idle and the two
// thresholds, decide the panel power level. The input controller maps each level to display HAL
// calls (brightness restore / dim / panel-off). Monotonic: AWAKE below dim_ms, DIM in [dim, sleep),
// ASLEEP at/after sleep_ms. sleep_ms is assumed > dim_ms (the caller's constants guarantee it).
typedef enum { DISP_AWAKE = 0, DISP_DIM = 1, DISP_ASLEEP = 2 } disp_power_t;

static inline disp_power_t idle_power_for(uint32_t inactive_ms, uint32_t dim_ms, uint32_t sleep_ms) {
  if (inactive_ms >= sleep_ms) return DISP_ASLEEP;
  if (inactive_ms >= dim_ms)   return DISP_DIM;
  return DISP_AWAKE;
}
