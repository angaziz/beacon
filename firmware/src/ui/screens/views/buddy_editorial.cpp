#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "core/datastore.h"
#include "core/hub_task.h"

static lv_obj_t *s_slot, *s_status, *s_kicker, *s_tool, *s_cmdbox, *s_cmd, *s_deny, *s_approve, *s_idle;

static void decide_cb(lv_event_t* e) {
  long approve = (long)lv_event_get_user_data(e);
  if (approve == 0 && buddy_dismiss()) return;   // deny doubles as dismiss for a "too late" warning
  buddy_decide(approve != 0);
}

static lv_obj_t* mk_btn(lv_obj_t* page, const char* txt, lv_align_t al, long approve) {
  lv_obj_t* b = lv_label_create(page); lv_obj_add_style(b, &S.display, 0);
  if (approve) lv_obj_add_style(b, &S.accent, 0);
  lv_label_set_text(b, txt);
  lv_obj_align(b, al, al == LV_ALIGN_BOTTOM_LEFT ? SAFE_INSET : -SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(b, decide_cb, LV_EVENT_CLICKED, (void*)approve);
  return b;
}

static void build(lv_obj_t* page) {
  s_slot = build_header(page, "CLAUDE");
  s_status = lv_label_create(page); lv_obj_add_style(s_status, &S.slot, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 26);
  s_kicker = lv_label_create(page); lv_obj_add_style(s_kicker, &S.eyebrow, 0);
  lv_label_set_text(s_kicker, "PERMISSION -- APPROVE?"); lv_obj_align(s_kicker, LV_ALIGN_LEFT_MID, SAFE_INSET, -80);
  s_tool = lv_label_create(page); lv_obj_add_style(s_tool, &S.display, 0); lv_obj_align(s_tool, LV_ALIGN_LEFT_MID, SAFE_INSET, -30);
  s_cmdbox = lv_obj_create(page); lv_obj_remove_style_all(s_cmdbox);
  lv_obj_set_size(s_cmdbox, SCREEN_W - 2*SAFE_INSET, 56); lv_obj_align(s_cmdbox, LV_ALIGN_LEFT_MID, SAFE_INSET, 36);
  lv_obj_set_style_border_width(s_cmdbox, 1, 0); lv_obj_set_style_border_color(s_cmdbox, lv_color_hex(0x333333), 0);
  s_cmd = lv_label_create(s_cmdbox); lv_obj_add_style(s_cmd, &S.body, 0); lv_obj_center(s_cmd);
  s_deny = mk_btn(page, "< DENY", LV_ALIGN_BOTTOM_LEFT, 0);
  s_approve = mk_btn(page, "APPROVE >", LV_ALIGN_BOTTOM_RIGHT, 1);
  s_idle = lv_label_create(page); lv_obj_add_style(s_idle, &S.slot, 0); lv_obj_center(s_idle);
}

static void show_prompt(bool on) {
  lv_obj_t* p[] = {s_kicker, s_tool, s_cmdbox, s_deny, s_approve};
  for (size_t i = 0; i < sizeof(p)/sizeof(p[0]); i++) {
    if (on) lv_obj_clear_flag(p[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(p[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (on) lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  buddy_rec_t b = ds_get_buddy(); uint32_t now = now_s();
  slot_set(s_slot, "REQ --", &b.hdr, now);
  txt_fmt(s_status, "%u RUNNING . %u WAITING . %uK TOK . CTX %u%%",
    b.running, b.waiting, (unsigned)(b.tokens/1000), b.context_pct);
  bool disabled = (b.hdr.state == ST_HUB_OFFLINE);
  if (b.prompt.present && !disabled) {
    show_prompt(true);
    txt_set(s_tool, b.prompt.tool);
    txt_set(s_cmd, b.prompt.hint);
    const beacon_theme_t* t = theme_active();
    switch (b.prompt.decision_state) {
    case PROMPT_PENDING:   // sent; both actions dim until the truthful ack (issue #8).
      txt_set(s_kicker, "SENT -- AWAITING");
      txt_color(s_kicker, t->accent);
      txt_set(s_deny, "< DENY");
      txt_color(s_deny, t->ink_dim);
      txt_color(s_approve, t->ink_dim);
      break;
    case PROMPT_SENT_OK:   // applied; held briefly before the tick clears (issue #12).
      txt_set(s_kicker, "SENT OK");
      txt_color(s_kicker, t->up);
      txt_set(s_deny, "< DENY");
      txt_color(s_deny, t->ink_dim);
      txt_color(s_approve, t->ink_dim);
      break;
    case PROMPT_TOO_LATE:   // did not apply; deny becomes the dismiss affordance.
      txt_set(s_kicker, "TOO LATE -- DIDN'T APPLY");
      txt_color(s_kicker, t->down);
      txt_set(s_deny, "< DISMISS");
      txt_color(s_deny, t->ink);
      txt_color(s_approve, t->ink_dim);
      break;
    default: {
      char badge[16]; buddy_queue_badge(b.prompt.queue_len, badge, sizeof(badge));
      char eb[48];
      snprintf(eb, sizeof(eb), "PERMISSION -- APPROVE?%s %us",
               badge, (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      txt_set(s_kicker, eb);
      txt_color(s_kicker, t->accent);
      txt_set(s_deny, "< DENY");
      txt_color(s_deny, t->ink_dim);
      txt_color(s_approve, t->accent);
      break;
    }
    }
  } else {
    show_prompt(false);
    if (disabled) txt_set(s_idle, "hub offline");
    else if (b.entry_count > 0) txt_set(s_idle, b.entries[0]);
    else txt_set(s_idle, "idle");
  }
}

extern const screen_view_t buddy_editorial_view = { build, update };
