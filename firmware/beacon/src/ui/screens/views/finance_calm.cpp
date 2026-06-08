// Dot-Matrix MARKETS view. Compact single-line rows: name (Inter, dim) | value (Doto figure) |
// change (up/down). Vertical scroll for the full instrument set. build() creates rows once;
// update() only sets text + color (no object creation / re-align). Chrome drawn by the carousel.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/fmt.h"
#include "config/layout.h"
#include "config/tickers.h"
#include "core/datastore.h"
#include <Arduino.h>
#include <time.h>
static void update(void);

static inline const char* fin_name(int i, const finance_rec_t& r) {
  return (i < DEFAULT_TICKERS_COUNT) ? DEFAULT_TICKERS[i].display_name : r.id;
}

#define FIN_ROWS_MAX 12
#define ROW_H        42

static lv_obj_t *s_status, *s_list;
static lv_obj_t *s_name[FIN_ROWS_MAX], *s_val[FIN_ROWS_MAX], *s_chg[FIN_ROWS_MAX];
static uint8_t   s_rows;

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dot = lv_obj_create(page);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(dot, t->accent, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 2, SAFE_INSET + 6);

  lv_obj_t* brand = lv_label_create(page);
  lv_label_set_text(brand, "markets");
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

  s_list = lv_obj_create(page);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - SAFE_INSET - 70);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 34);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(s_list, 4, 0);

  s_rows = ds_get_finance_count();
  if (s_rows > FIN_ROWS_MAX) s_rows = FIN_ROWS_MAX;

  for (uint8_t i = 0; i < s_rows; i++) {
    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_name[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_name[i], t->f_body, 0);
    lv_obj_set_style_text_color(s_name[i], t->ink_dim, 0);
    lv_obj_set_style_text_letter_space(s_name[i], 2, 0);
    lv_label_set_text(s_name[i], "");
    lv_obj_align(s_name[i], LV_ALIGN_LEFT_MID, 0, 0);

    s_chg[i] = lv_label_create(row);
    lv_obj_set_style_text_font(s_chg[i], t->f_body, 0);
    lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    lv_obj_set_width(s_chg[i], 96);
    lv_obj_set_style_text_align(s_chg[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_chg[i], "--");
    lv_obj_align(s_chg[i], LV_ALIGN_RIGHT_MID, 0, 0);

    s_val[i] = lv_label_create(row);   // Doto figure, between name and change
    lv_obj_set_style_text_font(s_val[i], t->f_display, 0);
    lv_obj_set_style_text_color(s_val[i], t->ink, 0);
    lv_obj_set_width(s_val[i], 150);
    lv_obj_set_style_text_align(s_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_val[i], "--");
    lv_obj_align(s_val[i], LV_ALIGN_RIGHT_MID, -104, 0);
  }
  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  uint32_t now = now_s();
  bool any_chip = false;
  uint32_t newest = 0;   // freshest fetch across tickers => "last updated" age
  for (uint8_t i = 0; i < s_rows; i++) {
    finance_rec_t r = ds_get_finance(i);
    if (r.hdr.last_updated > newest) newest = r.hdr.last_updated;
    lv_label_set_text(s_name[i], fin_name(i, r));
    bool ph = sv_placeholder(r.hdr.state), dim = sv_dim(r.hdr.state);
    if (ph) {
      lv_label_set_text(s_val[i], "--"); lv_label_set_text(s_chg[i], "");
    } else {
      char vb[40]; fmt_value(vb, sizeof(vb), r.value); lv_label_set_text(s_val[i], vb);
      char cb[24]; int dir = fmt_change(cb, sizeof(cb), r.change_pct); lv_label_set_text(s_chg[i], cb);
      lv_obj_set_style_text_color(s_chg[i], dim ? t->ink_dim : (dir > 0 ? t->up : dir < 0 ? t->down : t->ink_dim), 0);
    }
    lv_obj_set_style_text_color(s_val[i], dim ? t->ink_dim : t->ink, 0);
    char sbuf[24];
    if (!any_chip && sv_status(sbuf, sizeof(sbuf), &r.hdr, now)) {
      lv_label_set_text(s_status, sbuf);
      lv_obj_set_style_text_color(s_status, sv_severe(r.hdr.state) ? t->down : t->ink_dim, 0);
      any_chip = true;
    }
  }
  if (!any_chip) {
    char sb[20];   // last-update wall-clock time (static between fetches; no per-second ticking)
    if (newest) { time_t tt = newest; struct tm lt; localtime_r(&tt, &lt); strftime(sb, sizeof(sb), "as of %H:%M", &lt); }
    else        snprintf(sb, sizeof(sb), "live");
    lv_label_set_text(s_status, sb); lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }
}

extern const screen_view_t finance_calm_view = { build, update };
