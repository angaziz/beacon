#pragma once
#include <stdbool.h>
#include <stdint.h>

// Generic list picker over DURATIONS (durations.h), modeled on theme_panel. Opened from a
// settings row; lists every preset (current marked), tapping one applies via on_pick and closes.
// Suspends carousel swipe while open; restored on close. One instance.
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*duration_pick_cb)(uint8_t idx);

void duration_panel_open(const char* title, uint8_t current, duration_pick_cb on_pick);
bool duration_panel_is_open(void);
void duration_panel_close(void);

#ifdef __cplusplus
}
#endif
