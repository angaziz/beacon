#pragma once
#include <lvgl.h>
#include "ui/theme.h"   // beacon_theme_t

// Theme-owned shared styles. Screens attach these via lv_obj_add_style ONLY (no per-object
// color/font setters), so a theme switch restyles every screen by mutating these + reporting.
typedef struct {
  lv_style_t screen;     // bg
  lv_style_t eyebrow;    // mono, accent ("BEACON / X")
  lv_style_t slot;       // mono, ink_dim (top-right status slot / hints / dim labels)
  lv_style_t display;    // display font, ink (titles / values)
  lv_style_t hero;       // hero font, ink (clock / big figures)
  lv_style_t body;       // body font, ink
  lv_style_t up;         // text_color = up
  lv_style_t down;       // text_color = down
  lv_style_t accent;     // text_color = accent
  lv_style_t hairline;   // bg = line (1px rule objects)
  lv_style_t dim;        // text_color = ink_dim (overlay to dim a stale value)
} app_styles_t;

extern app_styles_t S;

void styles_init(void);                         // lv_style_init all (call once, before first rebuild)
void styles_rebuild(const beacon_theme_t* t);   // refresh shared styles from a theme (carousel's theme hook calls this)
