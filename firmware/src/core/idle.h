#pragma once
#include <stdint.h>

// Pure idle decision (FR-PLAT-7). No Arduino/LVGL -- host-tested. The caller owns the
// activity clock (LVGL inactivity) and the effects (brightness/sleep). dim_ms/sleep_ms == 0
// means that stage is disabled ("Never"). Sleep takes precedence over dim past its threshold.
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { IDLE_ACTIVE = 0, IDLE_DIM = 1, IDLE_SLEEP = 2 } idle_phase_t;

idle_phase_t idle_eval(uint32_t inactive_ms, uint32_t dim_ms, uint32_t sleep_ms);

#ifdef __cplusplus
}
#endif
