#pragma once
#include <stdint.h>
#include "ui/gauge_style.h"

// LVGL-free catalog data (host-testable). theme.cpp maps this -> beacon_theme_t (lv_color_t/lv_font_t).
// DESIGN.md owns token VALUES; only Editorial has full hex there, so the other six are concrete
// realizations of DESIGN.md's named accents (tunable on hardware in the Task 9 demo). bg is always
// pure black (AMOLED off-pixels). The ids + gauge mapping + struct are the frozen part.

typedef struct { uint8_t r, g, b; } bt_rgb_t;

typedef struct {
  const char*   id;                 // canonical id (DESIGN.md)
  bt_rgb_t      bg, ink, ink_dim, line, accent, accent2, up, down, alert;
  gauge_style_t gauge;
  uint8_t       glow;               // 0..255
  uint8_t       radius;             // element corner radius (px)
  uint8_t       stroke_hair, stroke_med;
} theme_catalog_t;

#define THEME_COUNT 7

static const theme_catalog_t THEME_CATALOG[THEME_COUNT] = {
  // Editorial Index (default) — DESIGN.md exact values
  { "editorial",
    {0,0,0}, {244,243,239}, {116,114,108}, {36,36,34}, {255,74,43}, {255,74,43},
    {244,243,239}, {255,74,43}, {255,74,43}, GAUGE_BAR, 0, 0, 1, 2 },
  // Aerospace HUD — cyan + amber, concentric rings
  { "hud",
    {0,0,0}, {224,247,250}, {96,125,139}, {20,40,46}, {0,229,255}, {255,179,0},
    {0,229,255}, {255,82,82}, {255,179,0}, GAUGE_RING, 40, 2, 1, 2 },
  // Calm Futurism — faint red, sparse white-on-black, big figures
  { "calm",
    {0,0,0}, {238,238,238}, {120,120,120}, {30,30,30}, {224,86,74}, {224,86,74},
    {238,238,238}, {224,86,74}, {224,86,74}, GAUGE_BIGFIG, 10, 8, 1, 2 },
  // Blueprint — draftsman blue, dimension lines
  { "blueprint",
    {0,0,0}, {214,230,245}, {90,120,150}, {26,42,58}, {74,144,217}, {74,144,217},
    {120,200,140}, {230,120,110}, {74,144,217}, GAUGE_MEASURE, 0, 0, 1, 2 },
  // LED Matrix — amber lit-pixel
  { "led",
    {0,0,0}, {255,176,0}, {120,82,0}, {40,28,0}, {255,176,0}, {255,176,0},
    {255,176,0}, {255,80,40}, {255,80,40}, GAUGE_CELL, 60, 0, 1, 2 },
  // Oscilloscope — phosphor green graticule + trace
  { "oscilloscope",
    {0,0,0}, {51,255,102}, {28,110,56}, {16,48,24}, {51,255,102}, {51,255,102},
    {51,255,102}, {255,90,90}, {255,210,80}, GAUGE_WAVEFORM, 50, 0, 1, 2 },
  // Analog Neo — ice blue, analog hands + sub-dials
  { "analog",
    {0,0,0}, {191,227,242}, {96,128,144}, {28,40,48}, {159,208,232}, {159,208,232},
    {191,227,242}, {232,140,140}, {159,208,232}, GAUGE_SUBDIAL, 20, 12, 1, 2 },
};
