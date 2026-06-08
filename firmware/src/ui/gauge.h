#pragma once
#include <lvgl.h>
#include "ui/theme.h"

// Render a 0..100 level into `parent` using the theme's gauge style. Returns the created
// root object (caller positions/sizes it). bar/ring/cell/measure/bigfig are implemented;
// waveform (oscilloscope) + subdial (analog) are bespoke widgets deferred to their screens
// and render a labeled placeholder for now (DESIGN.md outlier widgets).
lv_obj_t* gauge_render(lv_obj_t* parent, const beacon_theme_t* th, uint8_t pct);
