#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/fmt.h"
#include "config/layout.h"
#include "config/ticker_table.h"
#include "core/datastore.h"
#include <Arduino.h>

// Human-readable ticker name for slot i. Result is consumed immediately by lv_label_set_text
// (it copies). Core-1 render thread only.
static const char* fin_name(int i, const finance_rec_t& r) {
  static ticker_runtime_t t;
  return ticker_table_get(i, &t) ? t.name : r.id;
}

// Analog Neo markets: thin geometric rows in the analog language -- lowercase eyebrow, ice-blue
// accent, hairline rules. Each row: id (dim mono) | value (display, ink) | change (up/down + glyph).
// Scrolls vertically when the ticker count exceeds the visible rows.


#define MAX_ROWS 16
#define ROW_H    52

static lv_obj_t *s_status, *s_list;
static lv_obj_t *s_id[MAX_ROWS], *s_val[MAX_ROWS], *s_chg[MAX_ROWS];
static uint8_t   s_row_count;

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "markets");
  lv_obj_set_style_text_font(eyebrow, t->f_mono, 0);
  lv_obj_set_style_text_color(eyebrow, t->ink_dim, 0);
  lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "live");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  // Scroll container inside the safe area, below the header.
  s_list = lv_obj_create(page);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - SAFE_INSET - 80);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_style_pad_row(s_list, 0, 0);
  lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

  uint8_t count = ds_get_finance_count();
  if (count > MAX_ROWS) count = MAX_ROWS;
  s_row_count = count;

  for (uint8_t i = 0; i < count; i++) {
    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_y(row, i * ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // hairline rule at the bottom of each row
    lv_obj_t* rule = lv_obj_create(row);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, lv_pct(100), t->stroke_hair);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, t->line, 0);

    s_id[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_id[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_id[i], t->ink_dim, 0);
    // Value is centered at +24 (row center 193 => ~217). Cap the name so a long user-configured
    // name ellipsizes instead of overlapping the centered value figure.
    lv_obj_set_width(s_id[i], 150);
    lv_label_set_long_mode(s_id[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_id[i], LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(s_id[i], "");

    s_val[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_val[i], t->f_display, 0);
    lv_obj_set_style_text_color(s_val[i], t->ink, 0);
    lv_obj_align(s_val[i], LV_ALIGN_CENTER, 24, 0);
    lv_label_set_text(s_val[i], "--");

    s_chg[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_chg[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    lv_obj_align(s_chg[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_chg[i], "--");
  }
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  uint32_t now = now_s();

  // Header chip: derive a screen-level state from the first slot (per-row chips would crowd).
  if (s_row_count > 0) {
    finance_rec_t f0 = ds_get_finance(0);
    char chip[24];
    if (sv_status(chip, sizeof(chip), &f0.hdr, now)) {
      lv_label_set_text(s_status, chip);
      lv_obj_set_style_text_color(s_status, sv_severe(f0.hdr.state) ? t->down : t->ink_dim, 0);
    } else {
      lv_label_set_text(s_status, "live");
      lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
    }
  }

  for (uint8_t i = 0; i < s_row_count; i++) {
    finance_rec_t f = ds_get_finance(i);
    lv_label_set_text(s_id[i], fin_name(i, f));

    lv_color_t vcol = sv_dim(f.hdr.state) ? t->ink_dim : t->ink;
    if (sv_placeholder(f.hdr.state)) {
      lv_label_set_text(s_val[i], "--");
      lv_label_set_text(s_chg[i], "--");
      lv_obj_set_style_text_color(s_val[i], vcol, 0);
      lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
      continue;
    }

    char vb[24]; fmt_value(vb, sizeof(vb), f.value);
    lv_label_set_text(s_val[i], vb);
    lv_obj_set_style_text_color(s_val[i], vcol, 0);

    char cb[24]; int dir = fmt_change(cb, sizeof(cb), f.change_pct);
    lv_label_set_text(s_chg[i], cb);
    lv_color_t ccol = sv_dim(f.hdr.state) ? t->ink_dim
                      : (dir > 0 ? t->up : (dir < 0 ? t->down : t->ink_dim));
    lv_obj_set_style_text_color(s_chg[i], ccol, 0);
  }
}

extern const screen_view_t finance_analog_view = { build, update };
