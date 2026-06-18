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

// LED Matrix / MARKETS: amber lit rows. id (dim caps) | value (lit figure) | change (^/v + pct).
// Scrollable vertical list when count > visible rows.

#define FIN_MAX 16

static lv_obj_t *s_status, *s_list;
static lv_obj_t *s_id[FIN_MAX], *s_val[FIN_MAX], *s_chg[FIN_MAX];
static uint8_t s_count;


static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;

  lv_obj_t* eb = lv_label_create(page);
  lv_label_set_text(eb, "BEACON / MARKETS");
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->accent, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_list = lv_obj_create(page);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_style_pad_row(s_list, SPACE_S, 0);

  s_count = ds_get_finance_count();
  if (s_count > FIN_MAX) s_count = FIN_MAX;

  for (uint8_t i = 0; i < s_count; i++) {
    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_id[i] = lv_label_create(row);
    lv_label_set_text(s_id[i], "--");
    lv_obj_set_style_text_font(s_id[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_id[i], t->ink_dim, 0);
    lv_obj_set_width(s_id[i], 120);
    // Cap (already 120-wide) so a long user-configured name ellipsizes within its flex column
    // instead of growing and pushing the value/change.
    lv_label_set_long_mode(s_id[i], LV_LABEL_LONG_DOT);

    s_val[i] = lv_label_create(row);
    lv_label_set_text(s_val[i], "--");
    lv_obj_set_style_text_font(s_val[i], t->f_display, 0);
    lv_obj_set_style_text_color(s_val[i], t->accent, 0);

    s_chg[i] = lv_label_create(row);
    lv_label_set_text(s_chg[i], "--");
    lv_obj_set_style_text_font(s_chg[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    lv_obj_set_width(s_chg[i], 110);
    lv_obj_set_style_text_align(s_chg[i], LV_TEXT_ALIGN_RIGHT, 0);
  }
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;
  uint32_t now = now_s();

  // Page header reflects the first live/non-live row's state as a coarse summary.
  bool any_chip = false;
  for (uint8_t i = 0; i < s_count; i++) {
    finance_rec_t r = ds_get_finance(i);

    char buf[40];
    if (sv_placeholder(r.hdr.state)) {
      lv_label_set_text(s_val[i], "--");
      lv_label_set_text(s_chg[i], "--");
      lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    } else {
      fmt_value(buf, sizeof(buf), r.value);
      lv_label_set_text(s_val[i], buf);
      int dir = fmt_change(buf, sizeof(buf), r.change_pct);
      lv_label_set_text(s_chg[i], buf);
      lv_color_t cc = (dir > 0) ? t->up : (dir < 0) ? t->down : t->ink_dim;
      if (sv_dim(r.hdr.state)) cc = t->ink_dim;
      lv_obj_set_style_text_color(s_chg[i], cc, 0);
    }
    lv_obj_set_style_text_color(s_val[i], sv_dim(r.hdr.state) ? t->ink_dim : t->accent, 0);

    lv_label_set_text(s_id[i], fin_name(i, r));

    if (!any_chip) {
      char st[24];
      if (sv_status(st, sizeof(st), &r.hdr, now)) {
        lv_label_set_text(s_status, st);
        lv_obj_set_style_text_color(s_status, sv_severe(r.hdr.state) ? t->down : t->ink_dim, 0);
        any_chip = true;
      }
    }
  }
  if (!any_chip) lv_label_set_text(s_status, "LIVE");
}

extern const screen_view_t finance_led_view = { build, update };
