#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/fmt.h"
#include "config/layout.h"
#include "config/tickers.h"
#include "core/datastore.h"
#include <Arduino.h>

// Human-readable ticker name for slot i (slot i == ticker i, guaranteed by datastore_init).
static inline const char* fin_name(int i, const finance_rec_t& r) {
  return (i < DEFAULT_TICKERS_COUNT) ? DEFAULT_TICKERS[i].display_name : r.id;
}

// Oscilloscope / Signal MARKETS. Scope-instrument language: phosphor-green mono rows,
// each ticker a "trace channel": id (left, dim eyebrow), value (mono figure), change
// readout colored up/down. Scrolls vertically when >6 assets.

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

#define FIN_MAX_ROWS 16

static lv_obj_t* s_list;
static lv_obj_t *s_id[FIN_MAX_ROWS], *s_val[FIN_MAX_ROWS], *s_chg[FIN_MAX_ROWS];
static uint8_t s_rows;

static void update(void);

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* hdr = lv_label_create(page);
  lv_label_set_text(hdr, "MARKETS . CH SCAN");
  lv_obj_set_style_text_color(hdr, t->ink_dim, 0);
  lv_obj_set_style_text_font(hdr, t->f_mono, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_list = lv_obj_create(page);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 32);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 28);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_style_pad_row(s_list, 6, 0);

  uint8_t count = ds_get_finance_count();
  if (count > FIN_MAX_ROWS) count = FIN_MAX_ROWS;
  s_rows = count;

  for (uint8_t i = 0; i < count; i++) {
    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 56);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // Scope channel divider hairline under each row.
    lv_obj_set_style_border_color(row, t->line, 0);
    lv_obj_set_style_border_width(row, t->stroke_hair, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_bottom(row, 6, 0);

    s_id[i] = lv_label_create(row);
    lv_obj_set_style_text_color(s_id[i], t->ink_dim, 0);
    lv_obj_set_style_text_font(s_id[i], t->f_mono, 0);
    lv_obj_align(s_id[i], LV_ALIGN_LEFT_MID, 0, 0);

    s_val[i] = lv_label_create(row);
    lv_obj_set_style_text_color(s_val[i], t->ink, 0);
    lv_obj_set_style_text_font(s_val[i], t->f_display, 0);
    lv_obj_align(s_val[i], LV_ALIGN_RIGHT_MID, -96, 0);
    lv_obj_set_style_text_align(s_val[i], LV_TEXT_ALIGN_RIGHT, 0);

    s_chg[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_chg[i], t->f_mono, 0);
    lv_obj_align(s_chg[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_align(s_chg[i], LV_TEXT_ALIGN_RIGHT, 0);
  }

  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  uint32_t now = now_s();

  for (uint8_t i = 0; i < s_rows; i++) {
    finance_rec_t r = ds_get_finance(i);
    lv_label_set_text(s_id[i], fin_name(i, r));

    bool ph = sv_placeholder(r.hdr.state);
    bool dim = sv_dim(r.hdr.state);

    char vbuf[40];
    if (ph) {
      lv_label_set_text(s_val[i], "--");
      lv_obj_set_style_text_color(s_val[i], t->ink_dim, 0);
      lv_label_set_text(s_chg[i], "--");
      lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
      continue;
    }

    fmt_value(vbuf, sizeof(vbuf), r.value);
    lv_label_set_text(s_val[i], vbuf);
    lv_obj_set_style_text_color(s_val[i], dim ? t->ink_dim : t->ink, 0);

    char cbuf[24];
    int dir = fmt_change(cbuf, sizeof(cbuf), r.change_pct);
    lv_label_set_text(s_chg[i], cbuf);
    lv_color_t cc = dim ? t->ink_dim : (dir > 0 ? t->up : (dir < 0 ? t->down : t->ink_dim));
    lv_obj_set_style_text_color(s_chg[i], cc, 0);
  }
}

extern const screen_view_t finance_oscilloscope_view = { build, update };
