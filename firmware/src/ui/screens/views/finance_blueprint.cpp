// Blueprint / Schematic - MARKETS. Each instrument is a dimension row: id callout on the
// left, value figure center-right, signed change at the far right with up/down color. Rows
// separated by hairline rules. >6 rows scroll vertically. Grid/reticle drawn by chrome.
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

#define MAX_ROWS 16

static lv_obj_t *s_status, *s_list;
static lv_obj_t *s_id[MAX_ROWS], *s_val[MAX_ROWS], *s_chg[MAX_ROWS], *s_rule[MAX_ROWS];
static uint8_t   s_count;

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dwg = lv_label_create(page);
  lv_label_set_text(dwg, "DWG. BEACON-003 / MARKETS");
  lv_obj_set_style_text_color(dwg, t->ink_dim, 0);
  lv_obj_set_style_text_font(dwg, t->f_mono, 0);
  lv_obj_align(dwg, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "POLL 30S");
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_count = ds_get_finance_count();
  if (s_count > MAX_ROWS) s_count = MAX_ROWS;

  // Scrollable list inside the safe inset (vertical only).
  s_list = lv_obj_create(page);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 40);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 40);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_list, 0, 0);

  const int row_h = 56;
  for (int i = 0; i < s_count; i++) {
    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), row_h);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_id[i] = lv_label_create(row);
    lv_obj_set_style_text_color(s_id[i], t->ink_dim, 0);
    lv_obj_set_style_text_font(s_id[i], t->f_mono, 0);
    lv_obj_align(s_id[i], LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(s_id[i], "----");

    s_chg[i] = lv_label_create(row);
    lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    lv_obj_set_style_text_font(s_chg[i], t->f_mono, 0);
    lv_obj_align(s_chg[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_chg[i], "- --.--%");

    s_val[i] = lv_label_create(row);
    lv_obj_set_style_text_color(s_val[i], t->ink, 0);
    lv_obj_set_style_text_font(s_val[i], t->f_display, 0);
    lv_obj_align(s_val[i], LV_ALIGN_RIGHT_MID, -96, 0);
    lv_label_set_text(s_val[i], "--");

    // Hairline rule under each row.
    s_rule[i] = lv_obj_create(row);
    lv_obj_remove_style_all(s_rule[i]);
    lv_obj_set_size(s_rule[i], lv_pct(100), t->stroke_hair);
    lv_obj_align(s_rule[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_rule[i], t->line, 0);
    lv_obj_set_style_bg_opa(s_rule[i], LV_OPA_COVER, 0);
  }
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  uint32_t now = now_s();

  // Header reflects worst non-live state across slots; default POLL label.
  bool any_chip = false;
  for (int i = 0; i < s_count; i++) {
    finance_rec_t r = ds_get_finance((uint8_t)i);
    bool dim = sv_dim(r.hdr.state);
    bool ph  = sv_placeholder(r.hdr.state);

    lv_label_set_text(s_id[i], fin_name(i, r));

    if (ph) {
      lv_label_set_text(s_val[i], "--");
      lv_label_set_text(s_chg[i], "- --.--%");
      lv_obj_set_style_text_color(s_chg[i], t->ink_dim, 0);
    } else {
      char vb[40]; fmt_value(vb, sizeof(vb), r.value);
      lv_label_set_text(s_val[i], vb);

      char cb[24]; int dir = fmt_change(cb, sizeof(cb), r.change_pct);
      lv_label_set_text(s_chg[i], cb);
      lv_color_t cc = dim ? t->ink_dim : (dir > 0 ? t->up : (dir < 0 ? t->down : t->ink_dim));
      lv_obj_set_style_text_color(s_chg[i], cc, 0);
    }
    lv_obj_set_style_text_color(s_val[i], dim ? t->ink_dim : t->ink, 0);

    char buf[32];
    if (!any_chip && sv_status(buf, sizeof(buf), &r.hdr, now)) {
      lv_label_set_text(s_status, buf);
      any_chip = true;
    }
  }
  if (!any_chip) lv_label_set_text(s_status, "POLL 30S");
}

extern const screen_view_t finance_blueprint_view = { build, update };
