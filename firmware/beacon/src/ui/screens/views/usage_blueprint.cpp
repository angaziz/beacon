// Blueprint / Schematic - LIMITS (AI usage). Horizontal MEASURE axes: each window is an
// axis line with end ticks + a base line + a diamond marker at pct. CLAUDE and CODEX, each
// 5H + 7D. pct<0 => "--" and NO marker/fill. Grid/reticle drawn by chrome; do NOT redraw.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

#define AXIS_W   386   // 466 - 2*SAFE_INSET
#define N_AXES   4

static lv_obj_t *s_status;
static lv_obj_t *s_axis[N_AXES];   // custom-draw objects, one per window
static lv_obj_t *s_lab[N_AXES];    // "CLAUDE . 5H"
static lv_obj_t *s_pct[N_AXES];    // "24%" / "--"
static lv_obj_t *s_rst[N_AXES];    // "rst 2h08" reset countdown
static int16_t   s_val[N_AXES];    // -1 => unavailable (no marker/fill)

static const char* AXIS_NAME[N_AXES] = {
  "CLAUDE . 5H", "CLAUDE . 7D", "CODEX . 5H", "CODEX . 7D"
};

static void axis_draw_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_area_t a; lv_obj_get_coords(o, &a);
  const beacon_theme_t* t = theme_active(); if (!t) return;

  int idx = (int)(intptr_t)lv_obj_get_user_data(o);
  int16_t pct = s_val[idx];

  lv_coord_t y_mid = (a.y1 + a.y2) / 2;

  // End-cap ticks (left + right vertical strokes spanning the axis height).
  lv_draw_line_dsc_t cap; lv_draw_line_dsc_init(&cap);
  cap.color = t->line; cap.width = t->stroke_med; cap.opa = LV_OPA_COVER;
  lv_point_t lc1 = {a.x1, a.y1}, lc2 = {a.x1, a.y2};
  lv_point_t rc1 = {a.x2, a.y1}, rc2 = {a.x2, a.y2};
  lv_draw_line(ctx, &cap, &lc1, &lc2);
  lv_draw_line(ctx, &cap, &rc1, &rc2);

  // Base line (dim full-width center rule).
  lv_draw_line_dsc_t base; lv_draw_line_dsc_init(&base);
  base.color = t->line; base.width = t->stroke_hair; base.opa = LV_OPA_COVER;
  lv_point_t b1 = {a.x1, y_mid}, b2 = {a.x2, y_mid};
  lv_draw_line(ctx, &base, &b1, &b2);

  if (pct < 0) return;   // unavailable: ticks + base only, no fill/marker

  lv_coord_t span = a.x2 - a.x1;
  lv_coord_t mx = a.x1 + (lv_coord_t)((int32_t)span * pct / 100);

  // Fill line from left cap to marker (accent, brighter center rule).
  lv_draw_line_dsc_t fill; lv_draw_line_dsc_init(&fill);
  fill.color = t->accent; fill.width = 2; fill.opa = LV_OPA_COVER;
  lv_point_t f1 = {a.x1, y_mid}, f2 = {mx, y_mid};
  lv_draw_line(ctx, &fill, &f1, &f2);

  // Marker: a vertical stroke at pct + a small diamond at the axis midline.
  lv_draw_line_dsc_t mk; lv_draw_line_dsc_init(&mk);
  mk.color = t->accent; mk.width = 2; mk.opa = LV_OPA_COVER;
  lv_point_t m1 = {mx, a.y1}, m2 = {mx, a.y2};
  lv_draw_line(ctx, &mk, &m1, &m2);

  lv_coord_t d = 5;  // diamond half-size
  lv_point_t dt = {mx, (lv_coord_t)(y_mid - d)};
  lv_point_t dr = {(lv_coord_t)(mx + d), y_mid};
  lv_point_t db = {mx, (lv_coord_t)(y_mid + d)};
  lv_point_t dl = {(lv_coord_t)(mx - d), y_mid};
  lv_draw_line(ctx, &mk, &dt, &dr);
  lv_draw_line(ctx, &mk, &dr, &db);
  lv_draw_line(ctx, &mk, &db, &dl);
  lv_draw_line(ctx, &mk, &dl, &dt);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dwg = lv_label_create(page);
  lv_label_set_text(dwg, "DWG. BEACON-002 / LIMITS");
  lv_obj_set_style_text_color(dwg, t->ink_dim, 0);
  lv_obj_set_style_text_font(dwg, t->f_mono, 0);
  lv_obj_align(dwg, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "POLL 30S");
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  const int top = 96;          // first axis block top
  const int block = 78;        // per-window vertical pitch
  for (int i = 0; i < N_AXES; i++) {
    s_val[i] = -1;
    int by = top + i * block;

    s_lab[i] = lv_label_create(page);
    lv_label_set_text(s_lab[i], AXIS_NAME[i]);
    lv_obj_set_style_text_color(s_lab[i], t->ink_dim, 0);
    lv_obj_set_style_text_font(s_lab[i], t->f_mono, 0);
    lv_obj_set_pos(s_lab[i], SAFE_INSET, by);

    s_pct[i] = lv_label_create(page);
    lv_label_set_text(s_pct[i], "--");
    lv_obj_set_style_text_color(s_pct[i], t->ink, 0);
    lv_obj_set_style_text_font(s_pct[i], t->f_display, 0);
    lv_obj_align(s_pct[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, by - 6);

    s_rst[i] = lv_label_create(page);
    lv_label_set_text(s_rst[i], "rst --");
    lv_obj_set_style_text_color(s_rst[i], t->ink_dim, 0);
    lv_obj_set_style_text_font(s_rst[i], t->f_mono, 0);
    lv_obj_align(s_rst[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, by + 52);

    s_axis[i] = lv_obj_create(page);
    lv_obj_remove_style_all(s_axis[i]);
    lv_obj_set_size(s_axis[i], AXIS_W, 24);
    lv_obj_set_pos(s_axis[i], SAFE_INSET, by + 26);
    lv_obj_clear_flag(s_axis[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(s_axis[i], (void*)(intptr_t)i);
    lv_obj_add_event_cb(s_axis[i], axis_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
  }

  // Scale label centered at the bottom: 0 --- 50 --- 100 %
  lv_obj_t* scale = lv_label_create(page);
  lv_label_set_text(scale, "0 --- 50 --- 100 %");
  lv_obj_set_style_text_color(scale, t->ink_dim, 0);
  lv_obj_set_style_text_font(scale, t->f_mono, 0);
  lv_obj_align(scale, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);
}

static void apply_window(int idx, const usage_window_t* w, bool dim, bool ph,
                         const beacon_theme_t* t, uint32_t now) {
  int16_t pct = (ph || w->pct < 0) ? -1 : w->pct;
  s_val[idx] = pct;
  if (pct < 0) {
    lv_label_set_text(s_pct[idx], "--");
  } else {
    char b[8]; snprintf(b, sizeof(b), "%d%%", pct);
    lv_label_set_text(s_pct[idx], b);
  }
  lv_obj_set_style_text_color(s_pct[idx], dim ? t->ink_dim : t->ink, 0);
  char rb[12]; reset_str(rb, sizeof(rb), ph ? 0 : w->reset, now);
  lv_label_set_text_fmt(s_rst[idx], "rst %s", rb);
  lv_obj_invalidate(s_axis[idx]);
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  usage_rec_t u = ds_get_usage();
  uint32_t now = (uint32_t)(millis() / 1000);

  char buf[32];
  if (sv_status(buf, sizeof(buf), &u.hdr, now)) lv_label_set_text(s_status, buf);
  else lv_label_set_text(s_status, "POLL 30S");

  bool dim = sv_dim(u.hdr.state);
  bool ph  = sv_placeholder(u.hdr.state) || u.hdr.state == ST_HUB_OFFLINE;

  apply_window(0, &u.claude.h5, dim, ph, t, now);
  apply_window(1, &u.claude.d7, dim, ph, t, now);
  apply_window(2, &u.codex.h5,  dim, ph, t, now);
  apply_window(3, &u.codex.d7,  dim, ph, t, now);
}

extern const screen_view_t usage_blueprint_view = { build, update };
