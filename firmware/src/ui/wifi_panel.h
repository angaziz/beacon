#pragma once
#include <stdbool.h>

// Shared modal Wi-Fi panel (one instance, themed from the active tokens), drawn on lv_layer_top above
// the carousel. Opened by tapping any theme's Settings "Wi-Fi" row. Lists saved networks (active one
// marked), forgets a network (tap row -> confirm), and toggles Connect/Disconnect. Suspends carousel
// swipe while open; the explicit Back control restores it. Self-contained: holds no carousel page
// pointers (a theme switch can't reach it — it covers the theme picker).
#ifdef __cplusplus
extern "C" {
#endif
void wifi_panel_open(void);
bool wifi_panel_is_open(void);
#ifdef __cplusplus
}
#endif
