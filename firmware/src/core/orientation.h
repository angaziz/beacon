#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pure gravity-based screen-orientation detection. Fed the same accelerometer samples as
// core/imu_detect (g, plus a ms timestamp); emits a debounced quadrant. No I2C/Arduino -- host-tested.
//
// Convention: the returned value is the rotation the UI must apply so content reads upright, i.e.
// ORIENT_90 means "the device was turned 90 degrees clockwise, rotate the framebuffer to compensate".
// The accel-axis -> quadrant mapping lives in orient_feed and is the ONE place to fix if the panel
// turns out to be mounted differently than assumed (see ORIENT_NOTE there).
//
// Two guards keep it from flapping:
//   - flat guard: with the device face-up on a desk the in-plane gravity component is tiny and its
//     direction is noise, so below ORIENT_FLAT_G the current orientation is held.
//   - dwell + hysteresis: a new quadrant must dominate the other axis by ORIENT_HYST and hold for
//     ORIENT_DWELL_MS before it is committed.
#ifdef __cplusplus
extern "C" {
#endif

enum { ORIENT_0 = 0, ORIENT_90 = 1, ORIENT_180 = 2, ORIENT_270 = 3 };

void    orient_reset(uint8_t initial);                            // seed the committed orientation
void    orient_feed(float ax, float ay, float az, uint32_t t_ms);
uint8_t orient_stable(void);                                      // last committed quadrant
bool    orient_take_change(void);                                 // consume-on-read: true once per commit

#ifdef __cplusplus
}
#endif
