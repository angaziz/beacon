#pragma once
#include <stdbool.h>

// Closes the topmost open modal overlay, if any. Used by the IMU shake gesture (FR-PLAT-6).
// Returns true if something was dismissed.
#ifdef __cplusplus
extern "C" {
#endif
bool ui_dismiss_top_overlay(void);
#ifdef __cplusplus
}
#endif
