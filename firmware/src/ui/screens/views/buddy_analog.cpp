#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include <Arduino.h>

// Analog Neo claude (coding buddy): minimal ice-blue layout. Status dimension line up top.
// Prompt present => eyebrow + tool figure + thin-stroke hint box + DENY|APPROVE rule; else
// idle => recent entry or "idle". Decide routes through buddy_decide; the prompt waits on the hub
// ack and clears only on ok:true, else warns "too late" with a dismiss. Actions lock on hub-offline.


static lv_obj_t *s_status, *s_stat;
static lv_obj_t *s_eyebrow, *s_tool, *s_hintbox, *s_hint;
static lv_obj_t *s_deny, *s_approve, *s_actrule;
static lv_obj_t *s_idle;

static bool actions_locked(screen_state_t st) {
  return st == ST_HUB_OFFLINE;
}

static void decide_cb(lv_event_t* e) {
  bool approve = (bool)(intptr_t)lv_event_get_user_data(e);
  if (!approve && buddy_dismiss()) return;   // deny doubles as dismiss for a "too late" warning
  buddy_decide(approve);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "claude");
  lv_obj_set_style_text_font(eyebrow, t->f_mono, 0);
  lv_obj_set_style_text_color(eyebrow, t->ink_dim, 0);
  lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "req ----");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_stat = lv_label_create(page);
  lv_label_set_text(s_stat, "- run . - wait . --k tok . ctx --%");
  lv_obj_set_style_text_font(s_stat, t->f_mono, 0);
  lv_obj_set_style_text_color(s_stat, t->ink_dim, 0);
  lv_obj_align(s_stat, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 26);

  s_eyebrow = lv_label_create(page);
  lv_label_set_text(s_eyebrow, "permission - approve?");
  lv_obj_set_style_text_font(s_eyebrow, t->f_mono, 0);
  lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);
  lv_obj_align(s_eyebrow, LV_ALIGN_LEFT_MID, SAFE_INSET, -64);

  s_tool = lv_label_create(page);
  lv_label_set_text(s_tool, "--");
  lv_obj_set_style_text_font(s_tool, t->f_display, 0);
  lv_obj_set_style_text_color(s_tool, t->ink, 0);
  lv_obj_align(s_tool, LV_ALIGN_LEFT_MID, SAFE_INSET, -20);

  // Hint box: thin geometric frame, transparent fill.
  s_hintbox = lv_obj_create(page);
  lv_obj_remove_style_all(s_hintbox);
  lv_obj_set_size(s_hintbox, SCREEN_W - 2 * SAFE_INSET, 56);
  lv_obj_align(s_hintbox, LV_ALIGN_LEFT_MID, SAFE_INSET, 36);
  lv_obj_clear_flag(s_hintbox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_hintbox, t->radius, 0);
  lv_obj_set_style_border_width(s_hintbox, t->stroke_hair, 0);
  lv_obj_set_style_border_color(s_hintbox, t->line, 0);
  lv_obj_set_style_pad_left(s_hintbox, SPACE_M, 0);

  s_hint = lv_label_create(s_hintbox);
  lv_label_set_text(s_hint, "--");
  lv_obj_set_style_text_font(s_hint, t->f_mono, 0);
  lv_obj_set_style_text_color(s_hint, t->ink_dim, 0);
  lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 0, 0);

  s_actrule = lv_obj_create(page);
  lv_obj_remove_style_all(s_actrule);
  lv_obj_set_size(s_actrule, SCREEN_W - 2 * SAFE_INSET, t->stroke_hair);
  lv_obj_align(s_actrule, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 30));
  lv_obj_set_style_bg_color(s_actrule, t->line, 0);
  lv_obj_set_style_bg_opa(s_actrule, LV_OPA_COVER, 0);

  s_deny = lv_label_create(page);
  lv_label_set_text(s_deny, "< deny");
  lv_obj_set_style_text_font(s_deny, t->f_mono, 0);
  lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
  lv_obj_align(s_deny, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_deny, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_deny, decide_cb, LV_EVENT_CLICKED, (void*)(intptr_t)false);

  s_approve = lv_label_create(page);
  lv_label_set_text(s_approve, "approve >");
  lv_obj_set_style_text_font(s_approve, t->f_mono, 0);
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_obj_align(s_approve, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_approve, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_approve, decide_cb, LV_EVENT_CLICKED, (void*)(intptr_t)true);

  s_idle = lv_label_create(page);
  lv_label_set_text(s_idle, "idle");
  lv_obj_set_style_text_font(s_idle, t->f_mono, 0);
  lv_obj_set_style_text_color(s_idle, t->ink_dim, 0);
  lv_obj_align(s_idle, LV_ALIGN_LEFT_MID, SAFE_INSET, 0);
}

static void show_prompt(bool on) {
  lv_obj_t* p[] = { s_eyebrow, s_tool, s_hintbox, s_actrule, s_deny, s_approve };
  for (lv_obj_t* o : p) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  if (on) lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
  else    lv_obj_clear_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  buddy_rec_t r = ds_get_buddy();
  uint32_t now = now_s();

  char buf[32];
  if (sv_status(buf, sizeof(buf), &r.hdr, now)) {
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, sv_severe(r.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    char rb[16]; snprintf(rb, sizeof(rb), "req %s", r.prompt.present ? r.prompt.id : "----");
    lv_label_set_text(s_status, rb);
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  bool ph = sv_placeholder(r.hdr.state);
  char sb[64];
  if (ph) snprintf(sb, sizeof(sb), "- run . - wait . --k tok . ctx --%%");
  else snprintf(sb, sizeof(sb), "%u run . %u wait . %uk tok . ctx %u%%",
                (unsigned)r.running, (unsigned)r.waiting,
                (unsigned)(r.tokens / 1000), (unsigned)r.context_pct);
  lv_label_set_text(s_stat, sb);

  show_prompt(r.prompt.present);

  if (r.prompt.present) {
    lv_label_set_text(s_tool, r.prompt.tool);
    lv_label_set_text(s_hint, r.prompt.hint);
    bool locked = actions_locked(r.hdr.state);
    switch (r.prompt.decision_state) {
    case PROMPT_PENDING:   // sent; both actions dim + non-clickable until the truthful ack (issue #8).
      lv_label_set_text(s_eyebrow, "sent - awaiting");
      lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      lv_label_set_text(s_deny, "< deny");
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_clear_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
      break;
    case PROMPT_SENT_OK:   // applied; held briefly + non-clickable before the tick clears (issue #12).
      lv_label_set_text(s_eyebrow, "sent ok");
      lv_obj_set_style_text_color(s_eyebrow, t->up, 0);
      lv_label_set_text(s_deny, "< deny");
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_clear_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
      break;
    case PROMPT_TOO_LATE:   // did not apply; deny becomes the dismiss affordance.
      lv_label_set_text(s_eyebrow, "too late - didn't apply");
      lv_obj_set_style_text_color(s_eyebrow, t->down, 0);
      lv_label_set_text(s_deny, "< dismiss");
      lv_obj_set_style_text_color(s_deny, t->ink, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
      break;
    default: {
      char eb[32];
      snprintf(eb, sizeof(eb), "approve? %us", (unsigned)buddy_prompt_secs_left(&r, uptime_s()));
      lv_label_set_text(s_eyebrow, eb);
      lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);
      lv_label_set_text(s_deny, "< deny");
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, locked ? t->ink_dim : t->accent, 0);
      if (locked) {
        lv_obj_clear_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
      } else {
        lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
      }
      break;
    }
    }
  } else {
    if (r.entry_count > 0) lv_label_set_text(s_idle, r.entries[0]);
    else lv_label_set_text(s_idle, "idle");
  }
}

extern const screen_view_t buddy_analog_view = { build, update };
