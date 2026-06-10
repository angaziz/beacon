#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include <Arduino.h>
#include <string.h>

// LED Matrix / CLAUDE: status line + permission prompt (tool lit, hint in a box, DENY|APPROVE)
// or idle activity entries. Decide routes through buddy_decide; the prompt waits on the hub ack
// ("SENT -- AWAITING") and clears only on ok:true, else warns "TOO LATE" with a dismiss.

static lv_obj_t *s_status, *s_stats;
static lv_obj_t *s_prompt_box, *s_prompt_lead, *s_tool, *s_hint;
static lv_obj_t *s_actions, *s_deny, *s_approve;
static lv_obj_t *s_idle;


static void deny_cb(lv_event_t*)    { if (!buddy_dismiss()) buddy_decide(false); }
static void approve_cb(lv_event_t*) { buddy_decide(true); }

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;

  lv_obj_t* eb = lv_label_create(page);
  lv_label_set_text(eb, "BEACON / CLAUDE");
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->accent, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_stats = lv_label_create(page);
  lv_label_set_text(s_stats, "--");
  lv_obj_set_style_text_font(s_stats, t->f_mono, 0);
  lv_obj_set_style_text_color(s_stats, t->ink_dim, 0);
  lv_obj_align(s_stats, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 28);

  // Prompt layout.
  lv_obj_t* col = lv_obj_create(page);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, SCREEN_W - 2 * SAFE_INSET, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, SPACE_S, 0);
  lv_obj_align(col, LV_ALIGN_CENTER, 0, -10);
  s_prompt_box = col;

  s_prompt_lead = lv_label_create(col);
  lv_label_set_text(s_prompt_lead, "PERMISSION -- APPROVE?");
  lv_obj_set_style_text_font(s_prompt_lead, t->f_mono, 0);
  lv_obj_set_style_text_color(s_prompt_lead, t->accent, 0);

  s_tool = lv_label_create(col);
  lv_label_set_text(s_tool, "--");
  lv_obj_set_style_text_font(s_tool, t->f_display, 0);
  lv_obj_set_style_text_color(s_tool, t->accent, 0);

  lv_obj_t* box = lv_obj_create(col);
  lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, t->stroke_med, 0);
  lv_obj_set_style_border_color(box, t->line, 0);
  lv_obj_set_style_radius(box, t->radius, 0);
  lv_obj_set_style_pad_all(box, SPACE_M, 0);
  s_hint = lv_label_create(box);
  lv_label_set_text(s_hint, "--");
  lv_obj_set_width(s_hint, lv_pct(100));
  lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(s_hint, t->f_mono, 0);
  lv_obj_set_style_text_color(s_hint, t->ink, 0);

  // Actions: DENY | APPROVE.
  s_actions = lv_obj_create(page);
  lv_obj_remove_style_all(s_actions);
  lv_obj_set_size(s_actions, SCREEN_W - 2 * SAFE_INSET, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(s_actions, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);

  s_deny = lv_label_create(s_actions);
  lv_label_set_text(s_deny, "< DENY");
  lv_obj_set_style_text_font(s_deny, t->f_mono, 0);
  lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
  lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_deny, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_deny, deny_cb, LV_EVENT_CLICKED, NULL);

  s_approve = lv_label_create(s_actions);
  lv_label_set_text(s_approve, "APPROVE >");
  lv_obj_set_style_text_font(s_approve, t->f_mono, 0);
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_approve, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_approve, approve_cb, LV_EVENT_CLICKED, NULL);

  // Idle layout (activity log).
  s_idle = lv_label_create(page);
  lv_label_set_text(s_idle, "idle");
  lv_obj_set_width(s_idle, SCREEN_W - 2 * SAFE_INSET);
  lv_obj_set_style_text_font(s_idle, t->f_mono, 0);
  lv_obj_set_style_text_color(s_idle, t->ink_dim, 0);
  lv_obj_align(s_idle, LV_ALIGN_CENTER, 0, 0);
}

static void show(lv_obj_t* o, bool v) {
  if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;
  buddy_rec_t b = ds_get_buddy();
  uint32_t now = now_s();

  char st[24];
  if (sv_status(st, sizeof(st), &b.hdr, now)) {
    lv_label_set_text(s_status, st);
    lv_obj_set_style_text_color(s_status, sv_severe(b.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    lv_label_set_text(s_status, "LIVE");
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  char line[64];
  snprintf(line, sizeof(line), "%uR %uW %uK CTX%u%%",
           (unsigned)b.running, (unsigned)b.waiting,
           (unsigned)(b.tokens / 1000), (unsigned)b.context_pct);
  lv_label_set_text(s_stats, line);

  bool prompt = b.prompt.present;
  show(s_prompt_box, prompt);
  show(s_actions, prompt);
  show(s_idle, !prompt);

  if (prompt) {
    lv_label_set_text(s_tool, b.prompt.tool[0] ? b.prompt.tool : "--");
    lv_label_set_text(s_hint, b.prompt.hint[0] ? b.prompt.hint : "--");
    bool locked = (b.hdr.state == ST_HUB_OFFLINE);
    switch (b.prompt.decision_state) {
    case PROMPT_PENDING:   // sent; both actions dim until the truthful ack (issue #8).
      lv_label_set_text(s_prompt_lead, "SENT -- AWAITING");
      lv_obj_set_style_text_color(s_prompt_lead, t->accent, 0);
      lv_label_set_text(s_deny, "< DENY");
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      break;
    case PROMPT_SENT_OK:   // applied; held briefly before the tick clears (issue #12).
      lv_label_set_text(s_prompt_lead, "SENT OK");
      lv_obj_set_style_text_color(s_prompt_lead, t->up, 0);
      lv_label_set_text(s_deny, "< DENY");
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      break;
    case PROMPT_TOO_LATE:   // did not apply; deny becomes the dismiss affordance.
      lv_label_set_text(s_prompt_lead, "TOO LATE -- DIDN'T APPLY");
      lv_obj_set_style_text_color(s_prompt_lead, t->down, 0);
      lv_label_set_text(s_deny, "< DISMISS");
      lv_obj_set_style_text_color(s_approve, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_deny, t->ink, 0);
      break;
    default: {
      char eb[32];
      snprintf(eb, sizeof(eb), "PERMISSION %us", (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      lv_label_set_text(s_prompt_lead, eb);
      lv_obj_set_style_text_color(s_prompt_lead, t->accent, 0);
      lv_label_set_text(s_deny, "< DENY");
      lv_obj_set_style_text_color(s_approve, locked ? t->ink_dim : t->accent, 0);
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);  // deny stays dim either way
      break;
    }
    }
  } else {
    if (b.entry_count == 0) {
      lv_label_set_text(s_idle, "idle");
    } else {
      char log[BUDDY_ENTRIES * (BUDDY_ENTRY_LEN + 2)];
      log[0] = 0;
      for (uint8_t i = 0; i < b.entry_count && i < BUDDY_ENTRIES; i++) {
        strncat(log, b.entries[i], sizeof(log) - strlen(log) - 1);
        if (i + 1 < b.entry_count && i + 1 < BUDDY_ENTRIES)
          strncat(log, "\n", sizeof(log) - strlen(log) - 1);
      }
      lv_label_set_text(s_idle, log);
    }
  }
}

extern const screen_view_t buddy_led_view = { build, update };
