#include "ui/screens/screen_usage.h"
#include "ui/screens/screen_common.h"
#include "ui/theme.h"
#include "core/datastore.h"

static lv_obj_t* s_slot;
struct Cell { lv_obj_t *pct, *bar, *resets; };
static Cell s_c5, s_c7, s_x5, s_x7;   // claude 5h/7d, codex 5h/7d

static Cell make_cell(lv_obj_t* page, int dx, int dy, const char* tag) {
  Cell c;
  lv_obj_t* t = lv_label_create(page); lv_obj_add_style(t, &S.slot, 0);
  lv_label_set_text(t, tag); lv_obj_align(t, LV_ALIGN_TOP_LEFT, dx, dy + 8);
  c.pct = lv_label_create(page); lv_obj_add_style(c.pct, &S.display, 0); lv_obj_align(c.pct, LV_ALIGN_TOP_LEFT, dx + 44, dy);
  c.bar = lv_bar_create(page); lv_obj_set_size(c.bar, 150, 4); lv_obj_align(c.bar, LV_ALIGN_TOP_LEFT, dx, dy + 38);
  lv_bar_set_range(c.bar, 0, 100);
  c.resets = lv_label_create(page); lv_obj_add_style(c.resets, &S.slot, 0); lv_obj_align(c.resets, LV_ALIGN_TOP_LEFT, dx, dy + 50);
  return c;
}

static void set_cell(Cell& c, const usage_window_t& w, const beacon_theme_t* th) {
  if (w.pct < 0) { lv_label_set_text(c.pct, "--"); lv_obj_add_flag(c.bar, LV_OBJ_FLAG_HIDDEN); }   // unavailable: no bar
  else {
    lv_label_set_text_fmt(c.pct, "%d%%", w.pct);
    lv_obj_clear_flag(c.bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(c.bar, w.pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(c.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c.bar, th->line, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c.bar, th->accent, LV_PART_INDICATOR);
  }
  lv_label_set_text(c.resets, "resets --");   // reset time needs the time service (P0-D)
}

static lv_obj_t* build(lv_obj_t* page) {
  s_slot = build_header(page, "LIMITS");
  lv_obj_t* title = lv_label_create(page); lv_obj_add_style(title, &S.display, 0);
  lv_label_set_text(title, "AI USAGE"); lv_obj_align(title, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 34);
  lv_obj_t* cl = lv_label_create(page); lv_obj_add_style(cl, &S.slot, 0); lv_label_set_text(cl, "CLAUDE");
  lv_obj_align(cl, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 90);
  lv_obj_t* cx = lv_label_create(page); lv_obj_add_style(cx, &S.slot, 0); lv_label_set_text(cx, "CODEX");
  lv_obj_align(cx, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 220);
  s_c5 = make_cell(page, SAFE_INSET,       SAFE_INSET + 120, "5H");
  s_c7 = make_cell(page, SAFE_INSET + 200, SAFE_INSET + 120, "7D");
  s_x5 = make_cell(page, SAFE_INSET,       SAFE_INSET + 250, "5H");
  s_x7 = make_cell(page, SAFE_INSET + 200, SAFE_INSET + 250, "7D");
  return page;
}

static void update(void) {
  usage_rec_t u = ds_get_usage(); const beacon_theme_t* th = theme_active(); uint32_t now = now_s();
  slot_set(s_slot, "POLL 30S", &u.hdr, now);
  set_cell(s_c5, u.claude.h5, th); set_cell(s_c7, u.claude.d7, th);
  set_cell(s_x5, u.codex.h5, th);  set_cell(s_x7, u.codex.d7, th);
}

const screen_module_t usage_module = {"LIMITS", build, update};
