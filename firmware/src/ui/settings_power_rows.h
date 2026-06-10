#pragma once
#include <stddef.h>

// Shared Dim/Sleep settings behaviour (used by every settings_*.cpp view). Opens the duration
// picker, persists the choice, and re-applies the live idle config. Views own only the rows.
#ifdef __cplusplus
extern "C" {
#endif

void settings_power_open_dim(void);     // wire as a row's LV_EVENT_CLICKED handler body
void settings_power_open_sleep(void);
void settings_power_dim_label(char* out, size_t cap);    // current dim preset label
void settings_power_sleep_label(char* out, size_t cap);  // current sleep preset label

#ifdef __cplusplus
}
#endif
