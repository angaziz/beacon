#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "ui/gauge_style.h"

// FROZEN runtime theme contract (tech.md §6 + DESIGN.md tokens). Screens read tokens only.
// Built at runtime from THEME_CATALOG (theme_catalog.h) + the resident fonts (theme.cpp).
typedef struct {
  const char*      id;
  lv_color_t       bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  const lv_font_t *f_hero;      // oversized figures (clock, big %): digits + :%°.,+-/ subset
  const lv_font_t *f_display, *f_body, *f_mono; // display=titles/section figures (full ASCII); body/mono per role
  gauge_style_t    gauge;
  uint8_t          glow;
  uint8_t          radius;
  uint8_t          stroke_hair, stroke_med;
} beacon_theme_t;

// Global (not per-theme) tokens, DESIGN.md.
#define SPACE_XS   4
#define SPACE_S    8
#define SPACE_M   12
#define SPACE_L   16
#define SPACE_XL  24
#define SPACE_XXL 32
#define DUR_FAST  120   // ms
#define DUR       220
#define DUR_SLOW  400
// Easing is a single shared ease-out (no per-theme easing). Reduced-motion is a chunk-D
// Settings flag the motion helpers consult to crossfade/instant instead of animate.

// The active screen registers a rebuild hook; theme_set() invokes it after updating tokens so the
// screen tears down + rebuilds from the new theme (no reboot, FR-THEME-3). The screen's hook is
// responsible for the frozen teardown order: clear objects + remove styles, reset its lv_style_t,
// rebuild from the passed tokens (one-theme-resident, tech.md §6).
typedef void (*theme_apply_cb)(const beacon_theme_t*);
void theme_on_apply(theme_apply_cb cb);

void theme_set(uint8_t idx);                 // idx < THEME_COUNT; updates tokens + calls the apply hook
const beacon_theme_t* theme_active(void);    // current theme tokens (NULL before first theme_set)
uint8_t theme_index(void);                   // current index
