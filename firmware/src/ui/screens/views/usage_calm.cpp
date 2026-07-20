// Dot-Matrix LIMITS view. A clean 2x2 grid of self-labeled cells (row0 = provider 0's 5h|7d,
// row1 = provider 1's 5h|7d); each cell = tiny tag + big Doto figure (the figure IS the gauge) +
// faint "rst <countdown>". pct<0 => "--". Whole-screen HUB_OFFLINE via u.hdr. Chrome drawn by carousel.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "ui/screens/screen_common.h"
#include "core/datastore.h"
#include <Arduino.h>
static void update(void);


#define N_WIN 4   // p0.h5, p0.d7, p1.h5, p1.d7

static lv_obj_t *s_status;
static lv_obj_t *s_big[N_WIN], *s_sub[N_WIN], *s_tag[N_WIN];

static void mk_cell(lv_obj_t* page, const beacon_theme_t* t, int idx, int dx, int dy) {
  lv_obj_t* cell = lv_obj_create(page);
  lv_obj_remove_style_all(cell);
  lv_obj_set_size(cell, 150, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(cell, 2, 0);
  lv_obj_align(cell, LV_ALIGN_CENTER, dx, dy);

  lv_obj_t* tg = lv_label_create(cell);
  lv_label_set_text(tg, "");   // real text set in update() from provider label
  lv_obj_set_style_text_font(tg, t->f_body, 0);
  lv_obj_set_style_text_color(tg, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(tg, 3, 0);
  s_tag[idx] = tg;

  s_big[idx] = lv_label_create(cell);
  lv_obj_set_style_text_font(s_big[idx], t->f_display, 0);
  lv_obj_set_style_text_color(s_big[idx], t->ink, 0);
  lv_label_set_text(s_big[idx], "--");

  s_sub[idx] = lv_label_create(cell);
  lv_obj_set_style_text_font(s_sub[idx], t->f_body, 0);
  lv_obj_set_style_text_color(s_sub[idx], t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_sub[idx], 1, 0);
  lv_label_set_text(s_sub[idx], "rst --");
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dot = lv_obj_create(page);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(dot, t->accent, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 2, SAFE_INSET + 6);

  lv_obj_t* brand = lv_label_create(page);
  lv_label_set_text(brand, "usage");
  lv_obj_set_style_text_font(brand, t->f_body, 0);
  lv_obj_set_style_text_color(brand, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(brand, 4, 0);
  lv_obj_align(brand, LV_ALIGN_TOP_LEFT, SAFE_INSET + 18, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "live");
  lv_obj_set_style_text_font(s_status, t->f_body, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_status, 3, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -(SAFE_INSET + 2), SAFE_INSET);

  // 2x2 grid centered (slightly below header).
  mk_cell(page, t, 0, -82, -52);
  mk_cell(page, t, 1,  82, -52);
  mk_cell(page, t, 2, -82,  66);
  mk_cell(page, t, 3,  82,  66);
  update();
}

static void set_window(int idx, const usage_window_t* w, bool ph, bool dim,
                       const beacon_theme_t* t, uint32_t now) {
  char rb[12]; reset_str(rb, sizeof(rb), ph ? 0 : w->reset, now);
  lv_label_set_text_fmt(s_sub[idx], "rst %s", rb);
  if (ph || w->pct < 0) {
    lv_label_set_text(s_big[idx], "--");
    lv_obj_set_style_text_color(s_big[idx], t->ink_dim, 0);
  } else {
    char b[8]; snprintf(b, sizeof(b), "%d%%", w->pct);
    lv_label_set_text(s_big[idx], b);
    lv_obj_set_style_text_color(s_big[idx], dim ? t->ink_dim : t->ink, 0);
  }
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  usage_rec_t u = ds_get_usage();
  uint32_t now = now_s();
  bool ph = sv_placeholder(u.hdr.state), dim = sv_dim(u.hdr.state);
  usage_window_t none = usage_none();
  const usage_provider_t* p0 = usage_slot(&u, 0);
  const usage_provider_t* p1 = usage_slot(&u, 1);
  bool cdim = dim || (p0 && p0->stale), xdim = dim || (p1 && p1->stale);   // #108: dim last-good per provider.
  char tb[USAGE_LABEL_LEN + 8];
  usage_tag(tb, sizeof(tb), p0, " ", "5h", true); lv_label_set_text(s_tag[0], tb);
  usage_tag(tb, sizeof(tb), p0, " ", "7d", true); lv_label_set_text(s_tag[1], tb);
  usage_tag(tb, sizeof(tb), p1, " ", "5h", true); lv_label_set_text(s_tag[2], tb);
  usage_tag(tb, sizeof(tb), p1, " ", "7d", true); lv_label_set_text(s_tag[3], tb);
  set_window(0, p0 ? &p0->h5 : &none, ph, cdim, t, now);
  set_window(1, p0 ? &p0->d7 : &none, ph, cdim, t, now);
  set_window(2, p1 ? &p1->h5 : &none, ph, xdim, t, now);
  set_window(3, p1 ? &p1->d7 : &none, ph, xdim, t, now);
  char sbuf[24];
  if (sv_status(sbuf, sizeof(sbuf), &u.hdr, now)) {
    lv_label_set_text(s_status, sbuf);
    lv_obj_set_style_text_color(s_status, sv_severe(u.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "live");
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }
}

extern const screen_view_t usage_calm_view = { build, update };
