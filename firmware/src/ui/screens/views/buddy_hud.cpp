#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include <Arduino.h>

// Aerospace HUD / Claude. "// CLAUDE" eyebrow + telemetry status line
// (N RUNNING . N WAITING . NK TOK . CTX NN%). When a permission prompt is present:
// "PERMISSION - APPROVE?" + big tool name + hint box + DENY | APPROVE. Decide routes through
// buddy_decide; the prompt waits on the hub ack ("SENT - AWAITING") and clears only on ok:true,
// else warns "TOO LATE" with a dismiss. Otherwise idle: recent entries or "idle".


static lv_obj_t *s_status;       // top-right state chip
static lv_obj_t *s_tele;         // telemetry line
static lv_obj_t *s_prompt_box;   // prompt container (shown when present)
static lv_obj_t *s_idle_box;     // idle container (shown when !present)
static lv_obj_t *s_eyebrow;      // "PERMISSION - APPROVE?"
static lv_obj_t *s_tool;         // big tool name
static lv_obj_t *s_hint;         // command hint
static lv_obj_t *s_deny, *s_approve;
static lv_obj_t *s_entries[BUDDY_ENTRIES];

// In PROMPT_TOO_LATE the left action becomes "dismiss" (clear the warning); else it denies.
static void deny_cb(lv_event_t* e)    { (void)e; if (!buddy_dismiss()) buddy_decide(false); }
static void approve_cb(lv_event_t* e) { (void)e; buddy_decide(true); }

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* title = lv_label_create(page);
  lv_obj_add_style(title, &S.slot, 0);
  lv_label_set_text(title, "// CLAUDE");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_obj_add_style(s_status, &S.slot, 0);
  lv_label_set_text(s_status, "");
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_tele = lv_label_create(page);
  lv_obj_add_style(s_tele, &S.slot, 0);
  lv_label_set_text(s_tele, "-- RUNNING . -- WAITING");
  lv_obj_align(s_tele, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 24);

  // Prompt layout.
  s_prompt_box = lv_obj_create(page);
  lv_obj_remove_style_all(s_prompt_box);
  lv_obj_clear_flag(s_prompt_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_prompt_box, SCREEN_W - 2 * SAFE_INSET, 210);
  lv_obj_align(s_prompt_box, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_flex_flow(s_prompt_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_prompt_box, SPACE_M, 0);

  s_eyebrow = lv_label_create(s_prompt_box);
  lv_obj_add_style(s_eyebrow, &S.slot, 0);
  lv_obj_set_style_text_color(s_eyebrow, t->accent2, 0);
  lv_label_set_text(s_eyebrow, "PERMISSION - APPROVE?");

  s_tool = lv_label_create(s_prompt_box);
  lv_obj_set_style_text_font(s_tool, t->f_display, 0);
  lv_obj_set_style_text_color(s_tool, t->ink, 0);
  lv_label_set_text(s_tool, "--");

  lv_obj_t* hintbox = lv_obj_create(s_prompt_box);
  lv_obj_remove_style_all(hintbox);
  lv_obj_clear_flag(hintbox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(hintbox, lv_pct(100));
  lv_obj_set_height(hintbox, LV_SIZE_CONTENT);
  lv_obj_set_style_border_color(hintbox, t->line, 0);
  lv_obj_set_style_border_width(hintbox, 1, 0);
  lv_obj_set_style_pad_all(hintbox, SPACE_M, 0);
  s_hint = lv_label_create(hintbox);
  lv_obj_set_style_text_font(s_hint, t->f_mono, 0);
  lv_obj_set_style_text_color(s_hint, t->ink, 0);
  lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_hint, lv_pct(100));
  lv_label_set_text(s_hint, "--");

  // Action row: DENY (left) | APPROVE (right).
  lv_obj_t* actions = lv_obj_create(s_prompt_box);
  lv_obj_remove_style_all(actions);
  lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(actions, lv_pct(100));
  lv_obj_set_height(actions, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_top(actions, SPACE_S, 0);
  lv_obj_set_style_border_color(actions, t->line, 0);
  lv_obj_set_style_border_width(actions, 1, 0);
  lv_obj_set_style_border_side(actions, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  s_deny = lv_label_create(actions);
  lv_obj_add_style(s_deny, &S.slot, 0);
  lv_label_set_text(s_deny, "< DENY");
  lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_deny, deny_cb, LV_EVENT_CLICKED, NULL);

  s_approve = lv_label_create(actions);
  lv_obj_set_style_text_font(s_approve, t->f_mono, 0);
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_label_set_text(s_approve, "APPROVE >");
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_approve, approve_cb, LV_EVENT_CLICKED, NULL);

  // Idle layout.
  s_idle_box = lv_obj_create(page);
  lv_obj_remove_style_all(s_idle_box);
  lv_obj_clear_flag(s_idle_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_idle_box, SCREEN_W - 2 * SAFE_INSET, 200);
  lv_obj_align(s_idle_box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_flex_flow(s_idle_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_idle_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_idle_box, SPACE_M, 0);
  for (int i = 0; i < BUDDY_ENTRIES; i++) {
    s_entries[i] = lv_label_create(s_idle_box);
    lv_obj_add_style(s_entries[i], &S.slot, 0);
    lv_label_set_text(s_entries[i], "");
  }
}

static void update(void) {
  buddy_rec_t b = ds_get_buddy();
  uint32_t now = now_s();
  const beacon_theme_t* t = theme_active();

  char chip[24];
  if (sv_status(chip, sizeof(chip), &b.hdr, now)) {
    lv_label_set_text(s_status, chip);
    lv_obj_set_style_text_color(s_status, sv_severe(b.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "");
  }

  bool ph = sv_placeholder(b.hdr.state);
  if (ph) {
    lv_label_set_text(s_tele, "-- RUNNING . -- WAITING");
  } else {
    char tb[64];
    snprintf(tb, sizeof(tb), "%u RUNNING . %u WAITING . %uK TOK . CTX %u%%",
             (unsigned)b.running, (unsigned)b.waiting,
             (unsigned)(b.tokens / 1000), (unsigned)b.context_pct);
    lv_label_set_text(s_tele, tb);
  }

  bool show_prompt = b.prompt.present && !ph;
  if (show_prompt) {
    lv_obj_clear_flag(s_prompt_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_idle_box, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_tool, b.prompt.tool[0] ? b.prompt.tool : "--");
    lv_label_set_text(s_hint, b.prompt.hint[0] ? b.prompt.hint : "--");

    bool locked = (b.hdr.state == ST_HUB_OFFLINE || b.hdr.state == ST_RECONNECTING);
    switch (b.prompt.decision_state) {
    case PROMPT_PENDING:   // sent; both actions dim while we wait for the truthful ack (issue #8).
      lv_label_set_text(s_eyebrow, "SENT - AWAITING");
      lv_obj_set_style_text_color(s_eyebrow, t->accent2, 0);
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      break;
    case PROMPT_TOO_LATE:   // decision did not apply; warn + leave DENY as the dismiss affordance.
      lv_label_set_text(s_eyebrow, "TOO LATE - DIDN'T APPLY");
      lv_obj_set_style_text_color(s_eyebrow, t->down, 0);
      lv_label_set_text(s_deny, "< DISMISS");
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_deny, t->ink, 0);
      break;
    default:
      lv_label_set_text(s_eyebrow, "PERMISSION - APPROVE?");
      lv_obj_set_style_text_color(s_eyebrow, t->accent2, 0);
      lv_label_set_text(s_deny, "< DENY");
      lv_obj_set_style_text_color(s_approve, locked ? t->ink_dim : t->accent, 0);
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      break;
    }
  } else {
    lv_obj_add_flag(s_prompt_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_idle_box, LV_OBJ_FLAG_HIDDEN);

    uint8_t n = b.entry_count;
    if (n > BUDDY_ENTRIES) n = BUDDY_ENTRIES;
    for (int i = 0; i < BUDDY_ENTRIES; i++) {
      if (!ph && i < n) {
        lv_label_set_text(s_entries[i], b.entries[i]);
        lv_obj_clear_flag(s_entries[i], LV_OBJ_FLAG_HIDDEN);
      } else if (i == 0) {
        lv_label_set_text(s_entries[0], "idle");
        lv_obj_clear_flag(s_entries[0], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_entries[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

extern const screen_view_t buddy_hud_view = { build, update };
