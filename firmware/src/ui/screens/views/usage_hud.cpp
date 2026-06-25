#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// Aerospace HUD / AI Usage. "// AI LIMITS" eyebrow + FOUR concentric full-circle rings in a
// 2x2 grid (CLAUDE row cyan: 5H | 7D ; CODEX row amber: 5H | 7D). Each ring shows its window
// pct in the center and a "rst <countdown>" line below. pct<0 => "--" and no ring fill.
// Whole-screen HUB_OFFLINE via u.hdr.


#define RING_SZ 118
#define N_RINGS 4

static lv_obj_t *s_status;
static lv_obj_t *s_arc[N_RINGS];
static lv_obj_t *s_pct[N_RINGS];
static lv_obj_t *s_rst[N_RINGS];

static lv_obj_t* make_ring(lv_obj_t* parent, lv_color_t fill, const beacon_theme_t* t) {
  lv_obj_t* arc = lv_arc_create(parent);
  lv_obj_set_size(arc, RING_SZ, RING_SZ);
  lv_arc_set_rotation(arc, 270);          // start at 12 o'clock
  lv_arc_set_bg_angles(arc, 0, 360);      // full-circle track
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, t->line, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, fill, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 9, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 9, LV_PART_INDICATOR);
  return arc;
}

static void make_cell(lv_obj_t* page, int idx, int dx, int dy, lv_color_t fill,
                      const char* label, const beacon_theme_t* t) {
  s_arc[idx] = make_ring(page, fill, t);
  lv_obj_align(s_arc[idx], LV_ALIGN_CENTER, dx, dy);

  s_pct[idx] = lv_label_create(page);
  lv_obj_set_style_text_font(s_pct[idx], t->f_display, 0);
  lv_obj_set_style_text_color(s_pct[idx], t->ink, 0);
  lv_obj_set_width(s_pct[idx], RING_SZ);                              // fixed box centered on the ring
  lv_obj_set_style_text_align(s_pct[idx], LV_TEXT_ALIGN_CENTER, 0);   // stays centered as the text grows
  lv_label_set_text(s_pct[idx], "--");
  lv_obj_align_to(s_pct[idx], s_arc[idx], LV_ALIGN_CENTER, 0, -6);

  lv_obj_t* nm = lv_label_create(page);
  lv_obj_add_style(nm, &S.slot, 0);
  lv_obj_set_style_text_color(nm, fill, 0);
  lv_label_set_text(nm, label);
  lv_obj_align_to(nm, s_arc[idx], LV_ALIGN_CENTER, 0, 12);

  s_rst[idx] = lv_label_create(page);
  lv_obj_add_style(s_rst[idx], &S.slot, 0);
  lv_label_set_text(s_rst[idx], "rst --");
  lv_obj_align_to(s_rst[idx], s_arc[idx], LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* title = lv_label_create(page);
  lv_obj_add_style(title, &S.slot, 0);
  lv_label_set_text(title, "// AI LIMITS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_obj_add_style(s_status, &S.slot, 0);
  lv_label_set_text(s_status, "");
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  const int gx = 92;   // half horizontal gap between ring centers
  const int gy = 86;   // half vertical gap between ring rows
  const int oy = 12;   // grid vertical offset below the title

  // CLAUDE row (cyan / accent): 5H | 7D.
  make_cell(page, 0, -gx, -gy + oy, t->accent, "CL.5H", t);
  make_cell(page, 1,  gx, -gy + oy, t->accent, "CL.7D", t);
  // CODEX row (amber / accent2): 5H | 7D.
  make_cell(page, 2, -gx,  gy + oy, t->accent2, "CX.5H", t);
  make_cell(page, 3,  gx,  gy + oy, t->accent2, "CX.7D", t);
}

// Render one window into its ring + center pct + reset line. pct<0 => "--" + no fill.
static void set_window(int idx, const usage_window_t* w, bool placeholder, bool dim,
                       lv_color_t fill, const beacon_theme_t* t, uint32_t now) {
  char rb[12]; reset_str(rb, sizeof(rb), placeholder ? 0 : w->reset, now);
  lv_label_set_text_fmt(s_rst[idx], "rst %s", rb);

  if (placeholder || w->pct < 0) {
    lv_label_set_text(s_pct[idx], "--");
    lv_arc_set_value(s_arc[idx], 0);
    lv_obj_set_style_text_color(s_pct[idx], t->ink_dim, 0);
    return;
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (int)w->pct);
  lv_label_set_text(s_pct[idx], buf);
  lv_arc_set_value(s_arc[idx], (uint8_t)w->pct);
  lv_obj_set_style_text_color(s_pct[idx], dim ? t->ink_dim : t->ink, 0);
  lv_obj_set_style_arc_color(s_arc[idx], dim ? t->line : fill, LV_PART_INDICATOR);
}

static void update(void) {
  usage_rec_t u = ds_get_usage();
  uint32_t now = now_s();
  const beacon_theme_t* t = theme_active();

  char buf[24];
  if (sv_status(buf, sizeof(buf), &u.hdr, now)) {
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, sv_severe(u.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "");
  }

  bool ph = sv_placeholder(u.hdr.state);
  bool dim = sv_dim(u.hdr.state);
  bool cdim = dim || u.claude.stale, xdim = dim || u.codex.stale;   // #108: dim last-good per provider.

  set_window(0, &u.claude.h5, ph, cdim, t->accent,  t, now);
  set_window(1, &u.claude.d7, ph, cdim, t->accent,  t, now);
  set_window(2, &u.codex.h5,  ph, xdim, t->accent2, t, now);
  set_window(3, &u.codex.d7,  ph, xdim, t->accent2, t, now);
}

extern const screen_view_t usage_hud_view = { build, update };
