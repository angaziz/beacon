#include "ui/gauge.h"
#include <stdio.h>

#define GAUGE_W 220
#define GAUGE_H 120
#define CELL_COUNT 10

static lv_obj_t* make_root(lv_obj_t* parent) {
  lv_obj_t* g = lv_obj_create(parent);
  lv_obj_set_size(g, GAUGE_W, GAUGE_H);
  lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g, 0, 0);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  return g;
}

static void gauge_bar(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  lv_obj_t* bar = lv_bar_create(g);
  lv_obj_set_size(bar, GAUGE_W, 14);
  lv_obj_center(bar);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_radius(bar, th->radius, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, th->radius, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, th->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, th->accent, LV_PART_INDICATOR);
}

static void gauge_ring(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  lv_obj_t* arc = lv_arc_create(g);
  lv_obj_set_size(arc, GAUGE_H - 8, GAUGE_H - 8);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, pct);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, th->line, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, th->accent, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
}

static void gauge_cell(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  uint8_t lit = (uint8_t)((pct + 5) / 10);
  for (int i = 0; i < CELL_COUNT; i++) {
    lv_obj_t* c = lv_obj_create(g);
    lv_obj_set_size(c, 14, 36);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_color(c, (i < lit) ? th->accent : th->line, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  }
}

static void gauge_measure(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  // draftsman dimension: a flat bar with end-cap ticks + a numeric label
  lv_obj_t* bar = lv_bar_create(g);
  lv_obj_set_size(bar, GAUGE_W, 6);
  lv_obj_align(bar, LV_ALIGN_CENTER, 0, 8);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, th->line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, th->accent, LV_PART_INDICATOR);
  for (int i = 0; i < 3; i++) {  // ticks at 0/50/100
    lv_obj_t* tick = lv_obj_create(g);
    lv_obj_set_size(tick, th->stroke_med, 14);
    lv_obj_set_style_bg_color(tick, th->line, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_align(tick, LV_ALIGN_CENTER, (int16_t)(-GAUGE_W / 2 + i * (GAUGE_W / 2)), 8);
  }
  char buf[8]; snprintf(buf, sizeof(buf), "%u%%", pct);
  lv_obj_t* lbl = lv_label_create(g);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, th->ink, 0);
  lv_obj_set_style_text_font(lbl, th->f_mono, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -22);
}

static void gauge_bigfig(lv_obj_t* g, const beacon_theme_t* th, uint8_t pct) {
  char buf[8]; snprintf(buf, sizeof(buf), "%u%%", pct);
  lv_obj_t* lbl = lv_label_create(g);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, th->accent, 0);
  lv_obj_set_style_text_font(lbl, th->f_hero, 0);
  lv_obj_center(lbl);
}

static void gauge_stub(lv_obj_t* g, const beacon_theme_t* th, const char* what) {
  lv_obj_t* lbl = lv_label_create(g);
  lv_label_set_text(lbl, what);
  lv_obj_set_style_text_color(lbl, th->ink_dim, 0);
  lv_obj_set_style_text_font(lbl, th->f_mono, 0);
  lv_obj_center(lbl);
}

lv_obj_t* gauge_render(lv_obj_t* parent, const beacon_theme_t* th, uint8_t pct) {
  if (pct > 100) pct = 100;
  lv_obj_t* g = make_root(parent);
  switch (th->gauge) {
    case GAUGE_BAR:      gauge_bar(g, th, pct);     break;
    case GAUGE_RING:     gauge_ring(g, th, pct);    break;
    case GAUGE_CELL:     gauge_cell(g, th, pct);    break;
    case GAUGE_MEASURE:  gauge_measure(g, th, pct); break;
    case GAUGE_BIGFIG:   gauge_bigfig(g, th, pct);  break;
    case GAUGE_WAVEFORM: gauge_stub(g, th, "~ scope (P-later)"); break;  // bespoke, deferred
    case GAUGE_SUBDIAL:  gauge_stub(g, th, "(o) dial (P-later)"); break;  // bespoke, deferred
  }
  return g;
}
