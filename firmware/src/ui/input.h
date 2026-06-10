#pragma once
#include <stdbool.h>
// P3 input controller (FR-PLAT-5/6/7). Owns idle dim->sleep, wake (touch + IMU), and the richer
// touch/motion gestures, wiring the pure idle_policy + motion_gesture logic to the HAL and overlays.
// Runs entirely on Core-1: input_init() after carousel_init(); input_service() each loop() pass.
#ifdef __cplusplus
extern "C" {
#endif
void input_init(void);
void input_service(void);

// Consulted by the LVGL indev (lvgl_port.cpp): while the panel is asleep, a touch must wake it WITHOUT
// actuating the widget underneath, so the indev reports RELEASED and routes the finger here instead.
bool input_is_asleep(void);
void input_note_wake_touch(void);
#ifdef __cplusplus
}
#endif
