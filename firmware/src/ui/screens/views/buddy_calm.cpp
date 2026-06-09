// Calm Futurism CLAUDE (coding buddy) view. Sparse white-on-black. Top: [dot] claude + status
// slot. A dim stat line (running / waiting / tokens / ctx). prompt.present => big Doto tool name,
// a faint hint box, and DENY | APPROVE actions. Else => recent entries (or "idle").
// Decide routes through buddy_decide; the prompt waits on the hub ack and clears only on ok:true,
// else warns "too late" with a dismiss. Actions disabled when hub offline.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include <Arduino.h>
static void update(void);


static lv_obj_t *s_status, *s_stat;
static lv_obj_t *s_eyebrow, *s_tool, *s_hintbox, *s_hint;
static lv_obj_t *s_deny, *s_approve;
static lv_obj_t *s_idle[BUDDY_ENTRIES];
static bool      s_actions_enabled;

static void on_deny(lv_event_t* e) {
  (void)e;
  if (buddy_dismiss()) { update(); return; }   // clear a "too late" warning
  if (!s_actions_enabled) return;
  if (buddy_decide(false)) update();
}
static void on_approve(lv_event_t* e) {
  (void)e;
  if (!s_actions_enabled) return;
  if (buddy_decide(true)) update();
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dot = lv_obj_create(page);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, t->accent, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 4, SAFE_INSET + 12);

  lv_obj_t* brand = lv_label_create(page);
  lv_label_set_text(brand, "claude");
  lv_obj_set_style_text_font(brand, t->f_body, 0);
  lv_obj_set_style_text_color(brand, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(brand, 3, 0);
  lv_obj_align(brand, LV_ALIGN_TOP_LEFT, SAFE_INSET + 20, SAFE_INSET + 8);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "live");
  lv_obj_set_style_text_font(s_status, t->f_body, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_status, 3, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -(SAFE_INSET + 4), SAFE_INSET + 8);

  s_stat = lv_label_create(page);
  lv_label_set_text(s_stat, "--");
  lv_obj_set_style_text_font(s_stat, t->f_body, 0);
  lv_obj_set_style_text_color(s_stat, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_stat, 2, 0);
  lv_obj_align(s_stat, LV_ALIGN_TOP_LEFT, SAFE_INSET + 4, SAFE_INSET + 36);

  // Prompt layout (eyebrow + big tool name + hint box), centered.
  s_eyebrow = lv_label_create(page);
  lv_label_set_text(s_eyebrow, "");
  lv_obj_set_style_text_font(s_eyebrow, t->f_body, 0);
  lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_eyebrow, 3, 0);
  lv_obj_align(s_eyebrow, LV_ALIGN_CENTER, 0, -64);

  s_tool = lv_label_create(page);
  lv_obj_set_style_text_font(s_tool, t->f_display, 0);
  lv_obj_set_style_text_color(s_tool, t->ink, 0);
  lv_obj_align(s_tool, LV_ALIGN_CENTER, 0, -32);

  s_hintbox = lv_obj_create(page);
  lv_obj_remove_style_all(s_hintbox);
  lv_obj_set_size(s_hintbox, SCREEN_W - 2 * SAFE_INSET, 48);
  lv_obj_align(s_hintbox, LV_ALIGN_CENTER, 0, 28);
  lv_obj_set_style_border_width(s_hintbox, 1, 0);
  lv_obj_set_style_border_color(s_hintbox, t->line, 0);
  lv_obj_set_style_border_opa(s_hintbox, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_hintbox, t->radius, 0);
  lv_obj_set_style_pad_left(s_hintbox, SPACE_M, 0);

  s_hint = lv_label_create(s_hintbox);
  lv_label_set_text(s_hint, "");
  lv_obj_set_style_text_font(s_hint, t->f_mono, 0);
  lv_obj_set_style_text_color(s_hint, t->ink_dim, 0);
  lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 0, 0);

  s_deny = lv_label_create(page);
  lv_label_set_text(s_deny, "< deny");
  lv_obj_set_style_text_font(s_deny, t->f_body, 0);
  lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_deny, 2, 0);
  lv_obj_align(s_deny, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET + 4, -SAFE_INSET);
  lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_deny, 24);
  lv_obj_add_event_cb(s_deny, on_deny, LV_EVENT_CLICKED, NULL);

  s_approve = lv_label_create(page);
  lv_label_set_text(s_approve, "approve >");
  lv_obj_set_style_text_font(s_approve, t->f_body, 0);
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_obj_set_style_text_letter_space(s_approve, 2, 0);
  lv_obj_align(s_approve, LV_ALIGN_BOTTOM_RIGHT, -(SAFE_INSET + 4), -SAFE_INSET);
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_approve, 24);
  lv_obj_add_event_cb(s_approve, on_approve, LV_EVENT_CLICKED, NULL);

  // Idle entries (shown when no prompt), centered stack.
  for (uint8_t i = 0; i < BUDDY_ENTRIES; i++) {
    s_idle[i] = lv_label_create(page);
    lv_label_set_text(s_idle[i], "");
    lv_obj_set_style_text_font(s_idle[i], t->f_body, 0);
    lv_obj_set_style_text_color(s_idle[i], i == 0 ? t->ink : t->ink_dim, 0);
    lv_obj_set_style_text_letter_space(s_idle[i], 1, 0);
    lv_obj_align(s_idle[i], LV_ALIGN_CENTER, 0, -20 + i * 28);
  }

  update();
}

static void show_prompt(bool on) {
  lv_obj_t* p[] = { s_eyebrow, s_tool, s_hintbox, s_deny, s_approve };
  for (lv_obj_t* o : p) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  for (uint8_t i = 0; i < BUDDY_ENTRIES; i++) {
    if (on) lv_obj_add_flag(s_idle[i], LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_clear_flag(s_idle[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  buddy_rec_t b = ds_get_buddy();
  uint32_t now = now_s();

  bool offline = (b.hdr.state == ST_HUB_OFFLINE);
  s_actions_enabled = b.prompt.present && !offline;

  char sbuf[24];
  if (sv_status(sbuf, sizeof(sbuf), &b.hdr, now)) {
    lv_label_set_text(s_status, sbuf);
    lv_obj_set_style_text_color(s_status, sv_severe(b.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "live");
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  if (sv_placeholder(b.hdr.state)) {
    lv_label_set_text(s_stat, "--");
  } else {
    char st[48];
    snprintf(st, sizeof(st), "%u run  %u wait  %uK  ctx %u%%",
             (unsigned)b.running, (unsigned)b.waiting,
             (unsigned)(b.tokens / 1000), (unsigned)b.context_pct);
    lv_label_set_text(s_stat, st);
  }

  if (b.prompt.present) {
    show_prompt(true);
    lv_label_set_text(s_tool, b.prompt.tool[0] ? b.prompt.tool : "tool");
    lv_label_set_text(s_hint, b.prompt.hint);
    switch (b.prompt.decision_state) {
    case PROMPT_PENDING:   // sent; both actions dim until the truthful ack (issue #8).
      lv_label_set_text(s_eyebrow, "sent - awaiting");
      lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      lv_label_set_text(s_deny, "< deny");
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      break;
    case PROMPT_SENT_OK:   // applied; held briefly before the tick clears (issue #12).
      lv_label_set_text(s_eyebrow, "sent ok");
      lv_obj_set_style_text_color(s_eyebrow, t->up, 0);
      lv_label_set_text(s_deny, "< deny");
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      break;
    case PROMPT_TOO_LATE:   // did not apply; deny becomes the dismiss affordance.
      lv_label_set_text(s_eyebrow, "too late - didn't apply");
      lv_obj_set_style_text_color(s_eyebrow, t->down, 0);
      lv_label_set_text(s_deny, "< dismiss");
      lv_obj_set_style_text_color(s_deny, t->ink, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      break;
    default: {
      char eb[32];
      snprintf(eb, sizeof(eb), "approve? %us", (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      lv_label_set_text(s_eyebrow, eb);
      lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      lv_label_set_text(s_deny, "< deny");
      // Dim actions visually when disabled (hub offline).
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, s_actions_enabled ? t->accent : t->ink_dim, 0);
      break;
    }
    }
  } else {
    show_prompt(false);
    if (b.entry_count == 0) {
      lv_label_set_text(s_idle[0], "idle");
      for (uint8_t i = 1; i < BUDDY_ENTRIES; i++) lv_label_set_text(s_idle[i], "");
    } else {
      for (uint8_t i = 0; i < BUDDY_ENTRIES; i++)
        lv_label_set_text(s_idle[i], i < b.entry_count ? b.entries[i] : "");
    }
  }
}

extern const screen_view_t buddy_calm_view = { build, update };
