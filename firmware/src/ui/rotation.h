#pragma once
#include <stdint.h>
#include <stdbool.h>

// Screen-rotation policy (FR-PLAT-9 follow-up): decides WHICH rotation is active and WHEN a change is
// allowed to land. The mechanism -- framebuffer + touch remap -- is ui/lvgl_port.h; the debounced
// gravity quadrant is core/orientation.h. Persisted in NVS so the choice survives reboot.
//
// In ROTATION_AUTO the device follows gravity, which is the "stand it on its side so the charger
// clears the desk" case. A committed orientation change is deferred, not dropped, while the UI is
// mid-gesture (carousel snap animation or a finger still down): the swipe finishes, then the screen
// turns. That wait is bounded by the snap animation (<1s). The mascot's frame timer is suspended
// across the turn so it does not animate through the repaint.
#ifdef __cplusplus
extern "C" {
#endif

enum { ROTATION_AUTO = 4 };   // 0..3 are the fixed quadrants (core/orientation.h ORIENT_*)

void    rotation_init(void);        // load the persisted mode + apply it; call after carousel_init()
void    rotation_service(void);     // pump from loop() on Core 1, after imu_poll()

uint8_t rotation_mode(void);            // 0..3 fixed, or ROTATION_AUTO
void    rotation_set_mode(uint8_t m);   // persist + apply immediately

#ifdef __cplusplus
}
#endif
