#pragma once
#include <stdbool.h>

// Shared modal theme picker (one instance, themed from the active tokens), drawn on lv_layer_top
// above the carousel. Opened by tapping any theme's Settings "Theme" row. Lists every theme (active
// one marked); tapping a theme applies it (theme_set rebuilds the carousel pages) and closes. Suspends
// carousel swipe while open; the explicit Back control restores it. Self-contained: holds no carousel
// page pointers (a theme switch can't reach it -- it lives on lv_layer_top, above the rebuilt pages).
#ifdef __cplusplus
extern "C" {
#endif
void theme_panel_open(void);
bool theme_panel_is_open(void);
void theme_panel_close(void);
#ifdef __cplusplus
}
#endif
