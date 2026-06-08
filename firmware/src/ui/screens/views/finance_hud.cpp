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

// Aerospace HUD / Markets. "// MARKETS" eyebrow + vertical list of instrument rows:
// id (mono dim) | value (display, tabular) | change (up/down color + ^/v glyph). Hairline
// rule between rows. >6 rows scroll vertically. Per-row state via each slot's hdr.


#define MAX_ROWS 12

static lv_obj_t *s_status;
static lv_obj_t *s_id[MAX_ROWS], *s_val[MAX_ROWS], *s_chg[MAX_ROWS];
static lv_obj_t *s_rowobj[MAX_ROWS];
static uint8_t s_count;

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* title = lv_label_create(page);
  lv_obj_add_style(title, &S.slot, 0);
  lv_label_set_text(title, "// MARKETS");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_obj_add_style(s_status, &S.slot, 0);
  lv_label_set_text(s_status, "");
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_count = ds_get_finance_count();
  if (s_count > MAX_ROWS) s_count = MAX_ROWS;

  lv_obj_t* list = lv_obj_create(page);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 36);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 30);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(list, 0, 0);

  for (uint8_t i = 0; i < s_count; i++) {
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 56);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // hairline bottom rule
    lv_obj_set_style_border_color(row, t->line, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    s_rowobj[i] = row;

    s_id[i] = lv_label_create(row);
    lv_obj_add_style(s_id[i], &S.slot, 0);
    lv_label_set_text(s_id[i], "");

    s_val[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_val[i], t->f_display, 0);
    lv_obj_set_style_text_color(s_val[i], t->ink, 0);
    lv_obj_set_flex_grow(s_val[i], 1);
    lv_obj_set_style_text_align(s_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_val[i], "--");

    s_chg[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_chg[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    lv_obj_set_style_pad_left(s_chg[i], SPACE_M, 0);
    lv_obj_set_width(s_chg[i], 78);
    lv_obj_set_style_text_align(s_chg[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_chg[i], "--");
  }
}

static void update(void) {
  uint32_t now = now_s();
  const beacon_theme_t* t = theme_active();
  bool any_nonlive = false;
  char chip[24] = "";

  for (uint8_t i = 0; i < s_count; i++) {
    finance_rec_t r = ds_get_finance(i);
    lv_label_set_text(s_id[i], fin_name(i, r));

    bool ph = sv_placeholder(r.hdr.state);
    bool dim = sv_dim(r.hdr.state);

    if (sv_status(chip, sizeof(chip), &r.hdr, now)) any_nonlive = true;

    if (ph) {
      lv_label_set_text(s_val[i], "--");
      lv_label_set_text(s_chg[i], "--");
      lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    } else {
      char vb[24];
      fmt_value(vb, sizeof(vb), r.value);
      lv_label_set_text(s_val[i], vb);

      char cb[16];
      int dir = fmt_change(cb, sizeof(cb), r.change_pct);
      lv_label_set_text(s_chg[i], cb);
      lv_color_t cc = (dir > 0) ? t->up : (dir < 0) ? t->down : t->ink_dim;
      lv_obj_set_style_text_color(s_chg[i], dim ? t->ink_dim : cc, 0);
    }
    lv_obj_set_style_text_color(s_val[i], dim ? t->ink_dim : t->ink, 0);
  }

  if (any_nonlive) {
    lv_label_set_text(s_status, chip);
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "");
  }
}

extern const screen_view_t finance_hud_view = { build, update };
