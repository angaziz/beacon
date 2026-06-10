#include "core/idle.h"

idle_phase_t idle_eval(uint32_t inactive_ms, uint32_t dim_ms, uint32_t sleep_ms) {
  if (sleep_ms != 0 && inactive_ms >= sleep_ms) return IDLE_SLEEP;
  if (dim_ms   != 0 && inactive_ms >= dim_ms)   return IDLE_DIM;
  return IDLE_ACTIVE;
}
