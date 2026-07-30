#pragma once
#include <stddef.h>

// Shared Dim/Sleep/Wake settings behaviour (used by every settings_*.cpp view). Opens the picker,
// persists the choice, and re-applies the live idle config. Views own only the rows.
#ifdef __cplusplus
extern "C" {
#endif

void settings_power_open_dim(void);     // wire as a row's LV_EVENT_CLICKED handler body
void settings_power_open_sleep(void);
void settings_power_open_wake(void);    // auto-wake scope (issue #129)
void settings_power_dim_label(char* out, size_t cap);    // current dim preset label
void settings_power_sleep_label(char* out, size_t cap);  // current sleep preset label
void settings_power_wake_label(char* out, size_t cap);   // current wake-mode label

#ifdef __cplusplus
}
#endif
