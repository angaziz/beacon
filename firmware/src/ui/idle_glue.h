#pragma once
#include <stdbool.h>

// Bridges core/idle to the panel. Reads dim/sleep timeouts from NVS, watches LVGL's inactivity
// clock, and applies brightness (active -> dim -> off) on phase change. Touch resets LVGL
// inactivity for free (wake-on-touch); IMU wake calls lv_disp_trig_activity (later task).
#ifdef __cplusplus
extern "C" {
#endif

void idle_init(void);                  // call once after lvgl_port_begin()
void idle_service(void);               // call every loop() iteration
void idle_apply_config_from_nvs(void); // re-read timeouts after a settings change
bool idle_is_inactive(void);           // true while dimmed or asleep (first touch wakes only, no pass-through)

#ifdef __cplusplus
}
#endif
