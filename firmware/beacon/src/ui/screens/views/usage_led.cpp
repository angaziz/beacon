#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>

// LED Matrix / LIMITS: cell-meter bars. Each provider window is a row of 10 lit/unlit cells
// plus name caps + % figure. pct<0 => "--" and zero lit cells. HUB_OFFLINE via u.hdr.

#define METER_COUNT 4
#define CELLS 10

static lv_obj_t *s_status;
static lv_obj_t *s_pct[METER_COUNT];
static lv_obj_t *s_rst[METER_COUNT];
static lv_obj_t *s_cell[METER_COUNT][CELLS];


static const char* const METER_NAME[METER_COUNT] = {
  "CLAUDE 5H", "CLAUDE 7D", "CODEX 5H", "CODEX 7D"
};

static void make_meter(lv_obj_t* parent, const beacon_theme_t* t, int idx) {
  lv_obj_t* meter = lv_obj_create(parent);
  lv_obj_remove_style_all(meter);
  lv_obj_set_size(meter, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(meter, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(meter, SPACE_S, 0);

  lv_obj_t* head = lv_obj_create(meter);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

  lv_obj_t* nm = lv_label_create(head);
  lv_label_set_text(nm, METER_NAME[idx]);
  lv_obj_set_style_text_font(nm, t->f_mono, 0);
  lv_obj_set_style_text_color(nm, t->ink_dim, 0);

  s_rst[idx] = lv_label_create(head);
  lv_label_set_text(s_rst[idx], "rst --");
  lv_obj_set_style_text_font(s_rst[idx], t->f_mono, 0);
  lv_obj_set_style_text_color(s_rst[idx], t->ink_dim, 0);

  s_pct[idx] = lv_label_create(head);
  lv_label_set_text(s_pct[idx], "--");
  lv_obj_set_style_text_font(s_pct[idx], t->f_display, 0);
  lv_obj_set_style_text_color(s_pct[idx], t->accent, 0);

  lv_obj_t* cells = lv_obj_create(meter);
  lv_obj_remove_style_all(cells);
  lv_obj_set_size(cells, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cells, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(cells, SPACE_XS, 0);
  for (int c = 0; c < CELLS; c++) {
    lv_obj_t* cell = lv_obj_create(cells);
    lv_obj_remove_style_all(cell);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_height(cell, 18);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cell, t->line, 0);
    s_cell[idx][c] = cell;
  }
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;

  lv_obj_t* eb = lv_label_create(page);
  lv_label_set_text(eb, "BEACON / USAGE");
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->accent, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "LIVE");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  lv_obj_t* col = lv_obj_create(page);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < METER_COUNT; i++) make_meter(col, t, i);
}

static void set_meter(const beacon_theme_t* t, int idx, const usage_window_t* w, bool dim, uint32_t now) {
  char rb[12]; reset_str(rb, sizeof(rb), w->reset, now);
  lv_label_set_text_fmt(s_rst[idx], "rst %s", rb);
  bool avail = w->pct >= 0;
  if (!avail) {
    lv_label_set_text(s_pct[idx], "--");
    for (int c = 0; c < CELLS; c++) lv_obj_set_style_bg_color(s_cell[idx][c], t->line, 0);
    return;
  }
  int pct = w->pct; if (pct > 100) pct = 100;
  char buf[8]; snprintf(buf, sizeof(buf), "%d%%", pct);
  lv_label_set_text(s_pct[idx], buf);
  lv_obj_set_style_text_color(s_pct[idx], dim ? t->ink_dim : t->accent, 0);
  int lit = (pct + 5) / 10;
  lv_color_t on = dim ? t->ink_dim : t->accent;
  for (int c = 0; c < CELLS; c++)
    lv_obj_set_style_bg_color(s_cell[idx][c], (c < lit) ? on : t->line, 0);
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;
  usage_rec_t u = ds_get_usage();
  uint32_t now = now_s();

  char st[24];
  if (sv_status(st, sizeof(st), &u.hdr, now)) {
    lv_label_set_text(s_status, st);
    lv_obj_set_style_text_color(s_status, sv_severe(u.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "LIVE");
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  bool dim = sv_dim(u.hdr.state);
  bool ph = sv_placeholder(u.hdr.state);
  usage_window_t none = { -1, 0 };
  const usage_window_t* wins[METER_COUNT] = {
    &u.claude.h5, &u.claude.d7, &u.codex.h5, &u.codex.d7
  };
  for (int i = 0; i < METER_COUNT; i++)
    set_meter(t, i, ph ? &none : wins[i], dim, now);
}

extern const screen_view_t usage_led_view = { build, update };
