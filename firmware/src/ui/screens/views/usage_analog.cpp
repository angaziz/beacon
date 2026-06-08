#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// Analog Neo usage: FOUR conic SUB-DIALS in a 2x2 grid (claude 5H | 7D ; codex 5H | 7D). Each is
// a full-circle arc filled to pct in accent over a faint track, with the percent CENTERED inside
// the dial and a "rst <countdown>" line below. pct<0 => "--" and no fill (never feed <0 to the arc).


#define SUB_SIZE 116
#define N_DIAL 4

static lv_obj_t *s_status;
static lv_obj_t *s_arc[N_DIAL];
static lv_obj_t *s_pct[N_DIAL];
static lv_obj_t *s_rst[N_DIAL];

static void make_subdial(lv_obj_t* page, const beacon_theme_t* t, int idx, int dx, int dy,
                         const char* name) {
  lv_obj_t* arc = lv_arc_create(page);
  lv_obj_set_size(arc, SUB_SIZE, SUB_SIZE);
  lv_obj_align(arc, LV_ALIGN_CENTER, dx, dy);
  lv_arc_set_rotation(arc, 270);           // start indicator at 12 o'clock
  lv_arc_set_bg_angles(arc, 0, 360);       // full circle track
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, t->line, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, t->accent, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 9, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 9, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);   // drop default arc padding so center is true
  s_arc[idx] = arc;

  // Percent label is a CHILD of the arc and centered => sits at the geometric center of the dial.
  lv_obj_t* pct = lv_label_create(arc);
  lv_obj_set_style_text_font(pct, t->f_display, 0);
  lv_obj_set_style_text_color(pct, t->ink, 0);
  lv_label_set_text(pct, "--");
  lv_obj_center(pct);
  s_pct[idx] = pct;

  lv_obj_t* nm = lv_label_create(page);
  lv_label_set_text(nm, name);
  lv_obj_set_style_text_font(nm, t->f_mono, 0);
  lv_obj_set_style_text_color(nm, t->ink_dim, 0);
  lv_obj_align_to(nm, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

  s_rst[idx] = lv_label_create(page);
  lv_label_set_text(s_rst[idx], "rst --");
  lv_obj_set_style_text_font(s_rst[idx], t->f_mono, 0);
  lv_obj_set_style_text_color(s_rst[idx], t->ink_dim, 0);
  lv_obj_align_to(s_rst[idx], arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "usage");
  lv_obj_set_style_text_font(eyebrow, t->f_mono, 0);
  lv_obj_set_style_text_color(eyebrow, t->ink_dim, 0);
  lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "live");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  const int gx = 86;   // half horizontal gap between dial centers
  const int gy = 78;   // half vertical gap between dial rows
  const int oy = 18;   // grid vertical offset

  make_subdial(page, t, 0, -gx, -gy + oy, "claude 5h");
  make_subdial(page, t, 1,  gx, -gy + oy, "claude 7d");
  make_subdial(page, t, 2, -gx,  gy + oy, "codex 5h");
  make_subdial(page, t, 3,  gx,  gy + oy, "codex 7d");
}

static void apply(const beacon_theme_t* t, int idx, const usage_window_t* w,
                  bool ph, bool dim, uint32_t now) {
  char rb[12]; reset_str(rb, sizeof(rb), ph ? 0 : w->reset, now);
  lv_label_set_text_fmt(s_rst[idx], "rst %s", rb);

  int16_t v = (ph || w->pct < 0) ? -1 : w->pct;
  if (v < 0) {
    lv_arc_set_value(s_arc[idx], 0);              // unavailable => no fill, never feed <0
    lv_label_set_text(s_pct[idx], "--");
    lv_obj_set_style_text_color(s_pct[idx], t->ink_dim, 0);
    lv_obj_center(s_pct[idx]);
    return;
  }
  uint8_t clamped = v > 100 ? 100 : (uint8_t)v;
  lv_arc_set_value(s_arc[idx], clamped);
  char b[8]; snprintf(b, sizeof(b), "%u%%", clamped);
  lv_label_set_text(s_pct[idx], b);
  lv_obj_set_style_text_color(s_pct[idx], dim ? t->ink_dim : t->ink, 0);
  lv_obj_set_style_arc_color(s_arc[idx], dim ? t->ink_dim : t->accent, LV_PART_INDICATOR);
  lv_obj_center(s_pct[idx]);   // re-center after text width changes
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  usage_rec_t u = ds_get_usage();
  uint32_t now = now_s();

  char chip[24];
  if (sv_status(chip, sizeof(chip), &u.hdr, now)) {
    lv_label_set_text(s_status, chip);
    lv_obj_set_style_text_color(s_status, sv_severe(u.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "live");
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  bool dim = sv_dim(u.hdr.state);
  bool ph  = sv_placeholder(u.hdr.state);

  apply(t, 0, &u.claude.h5, ph, dim, now);
  apply(t, 1, &u.claude.d7, ph, dim, now);
  apply(t, 2, &u.codex.h5,  ph, dim, now);
  apply(t, 3, &u.codex.d7,  ph, dim, now);
}

extern const screen_view_t usage_analog_view = { build, update };
