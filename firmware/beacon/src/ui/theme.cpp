#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/fonts/fonts.h"

// Per-theme font selection (MANIFEST.md). mono is shared (JetBrains Mono) across all themes.
typedef struct { const lv_font_t *hero, *disp, *body, *mono; } theme_fonts_t;
static const theme_fonts_t THEME_FONTS[THEME_COUNT] = {
  {&font_sg_hero,    &font_sg_disp,    &font_sg_body,    &font_jbm_mono},  // editorial
  {&font_raj_hero,   &font_raj_disp,   &font_raj_body,   &font_jbm_mono},  // hud
  {&font_doto_hero,  &font_doto_disp,  &font_inter_body, &font_jbm_mono},  // calm
  {&font_cp_hero,    &font_cp_disp,    &font_cp_body,    &font_jbm_mono},  // blueprint
  {&font_pix_hero,   &font_pix_disp,   &font_inter_body, &font_jbm_mono},  // led
  {&font_jbm_hero,   &font_jbm_disp,   &font_jbm_body,   &font_jbm_mono},  // oscilloscope
  {&font_inter_hero, &font_inter_disp, &font_inter_body, &font_jbm_mono},  // analog
};

static beacon_theme_t s_theme;
static uint8_t        s_idx = 0;
static theme_apply_cb s_apply = nullptr;

static inline lv_color_t C(bt_rgb_t c) { return lv_color_make(c.r, c.g, c.b); }

void theme_on_apply(theme_apply_cb cb) { s_apply = cb; }

void theme_set(uint8_t idx) {
  if (idx >= THEME_COUNT) return;
  s_idx = idx;
  const theme_catalog_t* t = &THEME_CATALOG[idx];
  const theme_fonts_t*   f = &THEME_FONTS[idx];

  s_theme.id      = t->id;
  s_theme.bg      = C(t->bg);
  s_theme.ink     = C(t->ink);
  s_theme.ink_dim = C(t->ink_dim);
  s_theme.line    = C(t->line);
  s_theme.accent  = C(t->accent);
  s_theme.accent2 = C(t->accent2);
  s_theme.up      = C(t->up);
  s_theme.down    = C(t->down);
  s_theme.alert   = C(t->alert);
  s_theme.f_hero    = f->hero;
  s_theme.f_display = f->disp;
  s_theme.f_body    = f->body;
  s_theme.f_mono    = f->mono;
  s_theme.gauge       = t->gauge;
  s_theme.glow        = t->glow;
  s_theme.radius      = t->radius;
  s_theme.stroke_hair = t->stroke_hair;
  s_theme.stroke_med  = t->stroke_med;

  if (s_apply) s_apply(&s_theme);   // screen tears down + rebuilds from the new tokens
}

const beacon_theme_t* theme_active(void) { return s_theme.id ? &s_theme : nullptr; }
uint8_t theme_index(void) { return s_idx; }
