#pragma once
#include <stdbool.h>

// Full-screen animated mascot overlay, drawn on lv_layer_top above the carousel (see wifi_panel.h
// for the pattern this follows). Opened by swiping up or down on the CLAUDE/buddy screen
// (screen_buddy.cpp wires the open gesture); a second swipe up or down inside the overlay closes
// it (pal_panel.cpp wires its own close gesture). Tapping anywhere in the overlay (that isn't a
// swipe) plays a one-shot reaction animation -- see pal_panel.cpp for the state->animation
// mapping. Suspends carousel swipe while open; closing unconditionally restores it.
#ifdef __cplusplus
extern "C" {
#endif
void pal_panel_open(void);
void pal_panel_close(void);
bool pal_panel_is_open(void);
#ifdef __cplusplus
}
#endif
