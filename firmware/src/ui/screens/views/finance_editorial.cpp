#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "ui/fmt.h"
#include "config/ticker_table.h"
#include "core/datastore.h"

// Human-readable ticker name for slot i. Result is consumed immediately by txt_set (it copies).
// Core-1 render thread only.
static const char* fin_name(int i, const finance_rec_t& r) {
  static ticker_runtime_t t;
  return ticker_table_get(i, &t) ? t.name : r.id;
}

#define MAX_ROWS 16
static lv_obj_t* s_slot;
static lv_obj_t* s_list;
static struct { lv_obj_t *id, *val, *chg; } s_row[MAX_ROWS];
static int s_rows;

static void build(lv_obj_t* page) {
  s_slot = build_header(page, "MARKETS");
  s_list = lv_obj_create(page); lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2*SAFE_INSET, SCREEN_H - SAFE_INSET - 70);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 36);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(s_list, 10, 0);
  s_rows = ds_get_finance_count(); if (s_rows > MAX_ROWS) s_rows = MAX_ROWS;
  for (int i = 0; i < s_rows; i++) {
    lv_obj_t* r = lv_obj_create(s_list); lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100)); lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_row[i].id  = lv_label_create(r); lv_obj_add_style(s_row[i].id, &S.slot, 0);
    s_row[i].val = lv_label_create(r); lv_obj_add_style(s_row[i].val, &S.display, 0);
    s_row[i].chg = lv_label_create(r); lv_obj_add_style(s_row[i].chg, &S.slot, 0);
  }
}

static void update(void) {
  uint32_t now = now_s();
  record_hdr_t worst; worst.state = ST_LIVE; worst.err = ERR_NONE; worst.last_updated = now;
  for (int i = 0; i < s_rows; i++) {
    finance_rec_t f = ds_get_finance(i);
    txt_set(s_row[i].id, fin_name(i, f));
    if (sv_placeholder(f.hdr.state)) { txt_set(s_row[i].val, "--"); txt_set(s_row[i].chg, ""); }
    else {
      char v[40]; fmt_value(v, sizeof(v), f.value); txt_set(s_row[i].val, v);
      char c[16]; int dir = fmt_change(c, sizeof(c), f.change_pct); txt_set(s_row[i].chg, c);
      const beacon_theme_t* t = theme_active();
      if (t) txt_color(s_row[i].chg, dir > 0 ? t->up : dir < 0 ? t->down : t->ink_dim);
    }
    value_state(s_row[i].val, f.hdr.state);
    if (f.hdr.state != ST_LIVE) worst = f.hdr;
  }
  slot_set(s_slot, "MARKETS", &worst, now);
}

extern const screen_view_t finance_editorial_view = { build, update };
