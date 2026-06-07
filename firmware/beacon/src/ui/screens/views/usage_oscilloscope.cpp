#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// Oscilloscope / Signal LIMITS. FOUR scope CHANNELS in a 2x2 grid (CLAUDE 5H | 7D ; CODEX 5H | 7D),
// each a bordered scope box with a phosphor level fill rising from the bottom to pct (custom
// DRAW_MAIN rect + glow top line), a percent figure, and a "rst <countdown>" line. pct<0 (or
// loading) => "--" and NO fill. Dim values when stale/offline. HUB_OFFLINE via u.hdr.

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

#define N_CH 4

static lv_obj_t* s_status;
static lv_obj_t* s_pct[N_CH];
static lv_obj_t* s_rst[N_CH];
static lv_obj_t* s_box[N_CH];

// Per-box fill level 0..100; -1 => no fill (placeholder/unavailable).
static int16_t s_lvl[N_CH] = { -1, -1, -1, -1 };
static bool s_dim[N_CH] = { false, false, false, false };

static const char* const CH_NAME[N_CH] = {
  "CLAUDE . 5H", "CLAUDE . 7D", "CODEX . 5H", "CODEX . 7D"
};

static void update(void);

static void box_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  const beacon_theme_t* t = theme_active(); if (!t) return;

  int idx = (int)(intptr_t)lv_obj_get_user_data(o);
  int16_t lvl = s_lvl[idx];
  bool dim = s_dim[idx];
  if (lvl < 0) return;

  lv_area_t a; lv_obj_get_coords(o, &a);
  lv_coord_t h = lv_area_get_height(&a);
  lv_coord_t fill = (lv_coord_t)((int)h * (int)lvl / 100);
  if (fill < 1) return;

  lv_color_t col = dim ? t->ink_dim : t->accent;

  lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
  rd.bg_color = col; rd.bg_opa = LV_OPA_30; rd.radius = 0;
  lv_area_t fa = { a.x1, (lv_coord_t)(a.y2 - fill + 1), a.x2, a.y2 };
  lv_draw_rect(ctx, &rd, &fa);

  // Glowing top line of the fill (the signal level).
  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
  ld.color = col; ld.width = 2; ld.opa = LV_OPA_COVER;
  lv_point_t p1 = { a.x1, fa.y1 }, p2 = { a.x2, fa.y1 };
  lv_draw_line(ctx, &ld, &p1, &p2);
}

static void make_channel(lv_obj_t* parent, const beacon_theme_t* t, int idx) {
  lv_obj_t* ch = lv_obj_create(parent);
  lv_obj_remove_style_all(ch);
  lv_obj_set_size(ch, lv_pct(48), LV_SIZE_CONTENT);
  lv_obj_clear_flag(ch, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lab = lv_label_create(ch);
  lv_label_set_text(lab, CH_NAME[idx]);
  lv_obj_set_style_text_color(lab, t->ink_dim, 0);
  lv_obj_set_style_text_font(lab, t->f_mono, 0);
  lv_obj_align(lab, LV_ALIGN_TOP_LEFT, 0, 0);

  s_pct[idx] = lv_label_create(ch);
  lv_obj_set_style_text_color(s_pct[idx], t->ink, 0);
  lv_obj_set_style_text_font(s_pct[idx], t->f_display, 0);
  lv_label_set_text(s_pct[idx], "--");
  lv_obj_align(s_pct[idx], LV_ALIGN_TOP_RIGHT, 0, -2);

  s_box[idx] = lv_obj_create(ch);
  lv_obj_remove_style_all(s_box[idx]);
  lv_obj_set_size(s_box[idx], lv_pct(100), 64);
  lv_obj_align(s_box[idx], LV_ALIGN_TOP_LEFT, 0, 26);
  lv_obj_clear_flag(s_box[idx], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_color(s_box[idx], t->line, 0);
  lv_obj_set_style_border_width(s_box[idx], t->stroke_med, 0);
  lv_obj_set_style_radius(s_box[idx], 0, 0);
  lv_obj_set_user_data(s_box[idx], (void*)(intptr_t)idx);
  lv_obj_add_event_cb(s_box[idx], box_cb, LV_EVENT_DRAW_MAIN, NULL);

  s_rst[idx] = lv_label_create(ch);
  lv_label_set_text(s_rst[idx], "rst --");
  lv_obj_set_style_text_color(s_rst[idx], t->ink_dim, 0);
  lv_obj_set_style_text_font(s_rst[idx], t->f_mono, 0);
  lv_obj_align(s_rst[idx], LV_ALIGN_TOP_LEFT, 0, 94);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* hdr = lv_label_create(page);
  lv_label_set_text(hdr, "AI USAGE . LIVE");
  lv_obj_set_style_text_color(hdr, t->ink_dim, 0);
  lv_obj_set_style_text_font(hdr, t->f_mono, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "POLL 30S");
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* grid = lv_obj_create(page);
  lv_obj_remove_style_all(grid);
  lv_obj_set_size(grid, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(grid, LV_ALIGN_CENTER, 0, 18);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(grid, SPACE_M, 0);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < N_CH; i++) make_channel(grid, t, i);

  update();
}

static void apply_channel(const beacon_theme_t* t, int idx, const usage_window_t* w,
                          bool ph, bool dim, uint32_t now) {
  char rb[12]; reset_str(rb, sizeof(rb), ph ? 0 : w->reset, now);
  lv_label_set_text_fmt(s_rst[idx], "rst %s", rb);

  if (ph || w->pct < 0) {
    lv_label_set_text(s_pct[idx], "--");
    lv_obj_set_style_text_color(s_pct[idx], t->ink_dim, 0);
    s_lvl[idx] = -1;
  } else {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)w->pct);
    lv_label_set_text(s_pct[idx], buf);
    lv_obj_set_style_text_color(s_pct[idx], dim ? t->ink_dim : t->ink, 0);
    s_lvl[idx] = w->pct;
  }
  s_dim[idx] = dim;
  lv_obj_invalidate(s_box[idx]);
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  usage_rec_t u = ds_get_usage();
  uint32_t now = now_s();

  char chip[24];
  if (sv_status(chip, sizeof(chip), &u.hdr, now)) lv_label_set_text(s_status, chip);
  else lv_label_set_text(s_status, "POLL 30S");

  bool ph = sv_placeholder(u.hdr.state);
  bool dim = sv_dim(u.hdr.state);

  apply_channel(t, 0, &u.claude.h5, ph, dim, now);
  apply_channel(t, 1, &u.claude.d7, ph, dim, now);
  apply_channel(t, 2, &u.codex.h5,  ph, dim, now);
  apply_channel(t, 3, &u.codex.d7,  ph, dim, now);
}

extern const screen_view_t usage_oscilloscope_view = { build, update };
