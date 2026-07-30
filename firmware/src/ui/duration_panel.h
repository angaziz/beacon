#pragma once
#include <stdbool.h>
#include <stdint.h>

// Generic single-choice list picker, modeled on theme_panel. Opened from a settings row; lists
// every option (current marked), tapping one applies via on_pick and closes. Suspends carousel
// swipe while open; restored on close. One instance.
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*duration_pick_cb)(uint8_t idx);

// `labels` must outlive the panel (pass a static table). on_pick receives the chosen index.
void option_panel_open(const char* title, const char* const* labels, uint8_t count,
                       uint8_t current, duration_pick_cb on_pick);

void duration_panel_open(const char* title, uint8_t current, duration_pick_cb on_pick);  // DURATIONS
bool duration_panel_is_open(void);
void duration_panel_close(void);

#ifdef __cplusplus
}
#endif
