#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/screens/views/view_common.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include <Arduino.h>
#include <string.h>

// Oscilloscope / Signal CLAUDE. Scope-instrument language: status telemetry line, then either
// a permission PROMPT (tool name as the signal, hint in a bordered scope box, DENY|APPROVE
// readouts) or idle activity log. Decide routes through buddy_decide; the prompt waits on the hub
// ack and clears only on ok:true, else warns "too late" with a dismiss. Actions disabled on HUB_OFFLINE.


static lv_obj_t* s_telem;
static lv_obj_t* s_eyebrow;     // "PERMISSION - APPROVE?" / "ACTIVITY"
static lv_obj_t* s_tool;        // big tool name / "idle"
static lv_obj_t* s_box;         // hint scope box
static lv_obj_t* s_hint;        // hint text / activity lines
static lv_obj_t* s_deny;
static lv_obj_t* s_approve;

static void update(void);

static void decide_cb(lv_event_t* e) {
  bool approve = (bool)(intptr_t)lv_event_get_user_data(e);
  if (!approve && buddy_dismiss()) { update(); return; }   // deny doubles as dismiss for "too late"
  if (buddy_decide(approve)) update();
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* hdr = lv_label_create(page);
  lv_label_set_text(hdr, "CLAUDE . CH MON");
  lv_obj_set_style_text_color(hdr, t->ink_dim, 0);
  lv_obj_set_style_text_font(hdr, t->f_mono, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_telem = lv_label_create(page);
  lv_obj_set_style_text_color(s_telem, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_telem, t->f_mono, 0);
  lv_obj_align(s_telem, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 22);

  s_eyebrow = lv_label_create(page);
  lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);
  lv_obj_set_style_text_font(s_eyebrow, t->f_mono, 0);
  lv_obj_align(s_eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 64);

  s_tool = lv_label_create(page);
  lv_obj_set_style_text_color(s_tool, t->ink, 0);
  lv_obj_set_style_text_font(s_tool, t->f_display, 0);
  lv_obj_align(s_tool, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 88);

  s_box = lv_obj_create(page);
  lv_obj_remove_style_all(s_box);
  lv_obj_set_size(s_box, SCREEN_W - 2 * SAFE_INSET, 56);
  lv_obj_align(s_box, LV_ALIGN_CENTER, 0, 18);
  lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_color(s_box, t->line, 0);
  lv_obj_set_style_border_width(s_box, t->stroke_med, 0);
  lv_obj_set_style_radius(s_box, 0, 0);
  lv_obj_set_style_pad_all(s_box, 12, 0);

  s_hint = lv_label_create(s_box);
  lv_obj_set_style_text_color(s_hint, t->ink, 0);
  lv_obj_set_style_text_font(s_hint, t->f_mono, 0);
  lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_hint, SCREEN_W - 2 * SAFE_INSET - 24);
  lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 0, 0);

  s_deny = lv_label_create(page);
  lv_label_set_text(s_deny, "< DENY");
  lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_deny, t->f_mono, 0);
  lv_obj_align(s_deny, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_deny, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_deny, decide_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);

  s_approve = lv_label_create(page);
  lv_label_set_text(s_approve, "APPROVE >");
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_obj_set_style_text_font(s_approve, t->f_mono, 0);
  lv_obj_align(s_approve, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_approve, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_approve, decide_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);

  update();
}

static void show_actions(bool show, bool enabled, const beacon_theme_t* t) {
  if (show) {
    lv_obj_clear_flag(s_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_approve, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_deny, enabled ? t->ink_dim : t->line, 0);
    lv_obj_set_style_text_color(s_approve, enabled ? t->accent : t->line, 0);
  } else {
    lv_obj_add_flag(s_deny, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_approve, LV_OBJ_FLAG_HIDDEN);
  }
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  buddy_rec_t b = ds_get_buddy();
  uint32_t now = now_s();

  bool offline = (b.hdr.state == ST_HUB_OFFLINE);

  char chip[24];
  if (sv_status(chip, sizeof(chip), &b.hdr, now)) {
    lv_label_set_text(s_telem, chip);
  } else {
    char tl[64];
    buddy_stats_fmt(tl, sizeof(tl), &b, true);
    lv_label_set_text(s_telem, tl);
  }

  if (b.prompt.present) {
    lv_label_set_text(s_tool, b.prompt.tool[0] ? b.prompt.tool : "TOOL");
    lv_obj_set_style_text_font(s_tool, t->f_display, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, b.prompt.hint[0] ? b.prompt.hint : "--");
    switch (b.prompt.decision_state) {
    case PROMPT_PENDING:   // sent; both readouts dim until the truthful ack (issue #8).
      lv_label_set_text(s_eyebrow, "SENT - AWAITING");
      lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      lv_label_set_text(s_deny, "< DENY");
      show_actions(true, false, t);
      break;
    case PROMPT_SENT_OK:   // applied; held briefly before the tick clears (issue #12).
      lv_label_set_text(s_eyebrow, "SENT OK");
      lv_obj_set_style_text_color(s_eyebrow, t->up, 0);
      lv_label_set_text(s_deny, "< DENY");
      show_actions(true, false, t);
      break;
    case PROMPT_TOO_LATE:   // did not apply; deny becomes the dismiss readout.
      lv_label_set_text(s_eyebrow, "TOO LATE - DIDN'T APPLY");
      lv_obj_set_style_text_color(s_eyebrow, t->down, 0);
      lv_label_set_text(s_deny, "< DISMISS");
      lv_obj_clear_flag(s_deny, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_approve, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_text_color(s_deny, t->ink, 0);
      break;
    default: {
      char eb[32];
      snprintf(eb, sizeof(eb), "PERMISSION - APPROVE? %us", (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      lv_label_set_text(s_eyebrow, eb);
      lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);
      lv_label_set_text(s_deny, "< DENY");
      show_actions(true, !offline, t);
      break;
    }
    }
  } else {
    lv_label_set_text(s_eyebrow, "ACTIVITY");
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    show_actions(false, false, t);
    if (b.entry_count > 0) {
      char log[3 * (BUDDY_ENTRY_LEN + 2)];
      log[0] = 0;
      for (uint8_t i = 0; i < b.entry_count && i < BUDDY_ENTRIES; i++) {
        strncat(log, b.entries[i], sizeof(log) - strlen(log) - 1);
        if (i + 1 < b.entry_count && i + 1 < BUDDY_ENTRIES)
          strncat(log, "\n", sizeof(log) - strlen(log) - 1);
      }
      lv_label_set_text(s_tool, log);
      lv_obj_set_style_text_font(s_tool, t->f_mono, 0);
    } else {
      lv_label_set_text(s_tool, "idle");
      lv_obj_set_style_text_font(s_tool, t->f_display, 0);
    }
  }
}

extern const screen_view_t buddy_oscilloscope_view = { build, update };
