#include "ui/screens/screen_finance.h"
#include "ui/screens/screen_common.h"
#include "ui/fmt.h"
#include "core/datastore.h"

#define MAX_ROWS 16
static lv_obj_t* s_slot;
static lv_obj_t* s_list;
static struct { lv_obj_t *id, *val, *chg; } s_row[MAX_ROWS];
static int s_rows;

static lv_obj_t* build(lv_obj_t* page) {
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
  return page;
}

static void update(void) {
  uint32_t now = now_s();
  record_hdr_t worst; worst.state = ST_LIVE; worst.err = ERR_NONE; worst.last_updated = now;
  for (int i = 0; i < s_rows; i++) {
    finance_rec_t f = ds_get_finance(i);
    lv_label_set_text(s_row[i].id, f.id);
    if (sv_placeholder(f.hdr.state)) { lv_label_set_text(s_row[i].val, "--"); lv_label_set_text(s_row[i].chg, ""); }
    else {
      char v[40]; fmt_value(v, sizeof(v), f.value); lv_label_set_text(s_row[i].val, v);
      char c[16]; int dir = fmt_change(c, sizeof(c), f.change_pct); lv_label_set_text(s_row[i].chg, c);
      style_set(s_row[i].chg, &S.up, dir >= 0); style_set(s_row[i].chg, &S.down, dir < 0);
    }
    value_state(s_row[i].val, f.hdr.state);
    if (f.hdr.state != ST_LIVE) worst = f.hdr;   // last non-live row drives the header chip
  }
  slot_set(s_slot, "MARKETS", &worst, now);
}

const screen_module_t finance_module = {"MARKETS", build, update};
