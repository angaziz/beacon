#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/screens/views/view_common.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include "ui/idle_glue.h"
#include <Arduino.h>

// Analog Neo claude (coding buddy): minimal ice-blue layout. Status dimension line up top.
// Prompt present => eyebrow + tool figure + thin-stroke hint box + DENY|APPROVE rule; else
// idle => session list (up to SESSION_ROWS rows) or "idle". Decide routes through buddy_decide;
// the prompt waits on the hub ack and clears only on ok:true, else warns "too late" with a
// dismiss. Actions lock on hub-offline.

#define SESSION_ROWS    4
#define SESSION_ROW_H   64
#define SESSION_LIST_Y  (SAFE_INSET + 72)
// Tick heights: SHORT brackets the folder line; TALL spans down to the branch line.
#define TICK_SHORT      20
#define TICK_TALL       42

static lv_obj_t *s_status, *s_stat;
static lv_obj_t *s_eyebrow, *s_tool, *s_hintbox, *s_hint;
static lv_obj_t *s_deny, *s_approve, *s_actrule;
static lv_obj_t *s_row_folder[SESSION_ROWS];
static lv_obj_t *s_row_sub[SESSION_ROWS];
static lv_obj_t *s_row_tick[SESSION_ROWS];    // leading state bar
static lv_obj_t *s_row_state[SESSION_ROWS];   // right-aligned state word
static lv_obj_t *s_row_age[SESSION_ROWS];     // right-aligned age
static lv_obj_t *s_row_btn[SESSION_ROWS];     // transparent tap target (issue #110 Phase 2)
static lv_obj_t *s_q_btn, *s_q_badge;
static uint8_t   s_q_session_idx;
static void update(void);

static bool actions_locked(screen_state_t st) {
  return st == ST_HUB_OFFLINE;
}

static void decide_cb(lv_event_t* e) {
  if (idle_take_wake_tap()) return;
  bool approve = (bool)(intptr_t)lv_event_get_user_data(e);
  if (!approve && buddy_dismiss()) return;   // deny doubles as dismiss for a "too late" warning
  buddy_decide(approve);
}
static void on_row_tap(lv_event_t* e) {
  if (idle_take_wake_tap()) return;
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  buddy_rec_t b = ds_get_buddy();
  if (idx < b.session_count) buddy_open(b.sessions[idx].id);
}
static void on_question_tap(lv_event_t* e) {
  (void)e;
  if (idle_take_wake_tap()) return;
  buddy_rec_t b = ds_get_buddy();
  if (s_q_session_idx < b.session_count &&
      b.sessions[s_q_session_idx].state == BST_QUESTION) {
    buddy_open(b.sessions[s_q_session_idx].id);
    update();
  }
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "agents");
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

  for (uint8_t i = 0; i < SESSION_ROWS; i++) {
    lv_coord_t y = SESSION_LIST_Y + (lv_coord_t)(i * SESSION_ROW_H);

    // Transparent tap button created first (below labels in z-order) — issue #110 Phase 2.
    s_row_btn[i] = lv_obj_create(page);
    lv_obj_remove_style_all(s_row_btn[i]);
    lv_obj_set_size(s_row_btn[i], SCREEN_W - 2 * SAFE_INSET, SESSION_ROW_H);
    lv_obj_align(s_row_btn[i], LV_ALIGN_TOP_LEFT, SAFE_INSET, y);
    lv_obj_set_style_bg_opa(s_row_btn[i], LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_row_btn[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_row_btn[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_row_btn[i], on_row_tap, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    // Tick bar: 3px wide; top-anchored to the folder line. Height set per row in update()
    // (TICK_SHORT = folder line only; TICK_TALL = spans down to the branch line).
    s_row_tick[i] = lv_obj_create(page);
    lv_obj_remove_style_all(s_row_tick[i]);
    lv_obj_set_size(s_row_tick[i], 3, TICK_SHORT);
    lv_obj_set_style_bg_opa(s_row_tick[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_row_tick[i], t->ink_dim, 0);
    lv_obj_set_style_radius(s_row_tick[i], 1, 0);

    s_row_folder[i] = lv_label_create(page);
    lv_label_set_text(s_row_folder[i], "");
    lv_obj_set_style_text_font(s_row_folder[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_folder[i], t->ink, 0);
    lv_obj_set_width(s_row_folder[i], SCREEN_W - 2 * SAFE_INSET - 12 - 90);
    lv_label_set_long_mode(s_row_folder[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_folder[i], LV_ALIGN_TOP_LEFT, SAFE_INSET + 12, y);
    // Top-align the tick to the folder line; small +y nudge brackets the folder cap-height.
    lv_obj_align_to(s_row_tick[i], s_row_folder[i], LV_ALIGN_OUT_LEFT_TOP, -9, 4);

    s_row_sub[i] = lv_label_create(page);
    lv_label_set_text(s_row_sub[i], "");
    lv_obj_set_style_text_font(s_row_sub[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_sub[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_sub[i], SCREEN_W - 2 * SAFE_INSET - 12 - 90);
    lv_label_set_long_mode(s_row_sub[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_sub[i], LV_ALIGN_TOP_LEFT, SAFE_INSET + 12, y + 26);

    s_row_state[i] = lv_label_create(page);
    lv_label_set_text(s_row_state[i], "");
    lv_obj_set_style_text_font(s_row_state[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_state[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_state[i], 88);
    lv_label_set_long_mode(s_row_state[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_state[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, y);

    s_row_age[i] = lv_label_create(page);
    lv_label_set_text(s_row_age[i], "");
    lv_obj_set_style_text_font(s_row_age[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_age[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_age[i], 88);
    lv_label_set_long_mode(s_row_age[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_age[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, y + 26);
  }

  // Question-card tier: full-page tap target + badge for multi-question count.
  s_q_btn = lv_btn_create(page);
  lv_obj_remove_style_all(s_q_btn);
  lv_obj_set_size(s_q_btn, lv_pct(100), lv_pct(100));
  lv_obj_align(s_q_btn, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(s_q_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_q_btn, on_question_tap, LV_EVENT_CLICKED, NULL);

  s_q_badge = lv_label_create(page);
  lv_obj_set_style_text_font(s_q_badge, t->f_mono, 0);
  lv_obj_set_style_text_color(s_q_badge, t->ink_dim, 0);
  lv_label_set_text(s_q_badge, "");
  lv_obj_align(s_q_badge, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);
  lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
}

static void show_prompt(bool on) {
  lv_obj_t* p[] = { s_eyebrow, s_tool, s_hintbox, s_actrule, s_deny, s_approve };
  for (lv_obj_t* o : p) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  for (uint8_t i = 0; i < SESSION_ROWS; i++) {
    if (on) {
      lv_obj_add_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }
  }
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
  if (ph) snprintf(sb, sizeof(sb), "- run . - wait");
  else buddy_stats_fmt(sb, sizeof(sb), &r, false);
  lv_label_set_text(s_stat, sb);

  // Find newest question session (first in array = newest by hub ordering).
  int8_t q_idx = -1; uint8_t q_count = 0;
  for (uint8_t i = 0; i < r.session_count; i++) {
    if (r.sessions[i].state == BST_QUESTION) {
      if (q_idx < 0) q_idx = (int8_t)i;
      q_count++;
    }
  }

  lv_obj_add_flag(s_q_btn,   LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);

  if (r.prompt.present) {
    show_prompt(true);
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
      char badge[16]; buddy_queue_badge(r.prompt.queue_len, badge, sizeof(badge));
      char eb[48];
      snprintf(eb, sizeof(eb), "approve?%s %us",
               badge, (unsigned)buddy_prompt_secs_left(&r, uptime_s()));
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
  } else if (q_idx >= 0) {
    // TIER 2: question card — one or more sessions waiting for user input on mac.
    s_q_session_idx = (uint8_t)q_idx;
    const buddy_session_t* qs = &r.sessions[q_idx];

    show_prompt(true);
    lv_obj_add_flag(s_actrule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_deny,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_approve, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_eyebrow, "your turn");
    lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);

    char folder[BUDDY_LABEL_LEN], branch[BUDDY_LABEL_LEN];
    buddy_session_split_label(qs->label, folder, sizeof(folder), branch, sizeof(branch));
    lv_label_set_text(s_tool, folder[0] ? folder : qs->id);

    char hintbuf[BUDDY_HINT_LEN + 32];
    if (branch[0])
      snprintf(hintbuf, sizeof(hintbuf), "%s\ntap to answer on mac", branch);
    else
      snprintf(hintbuf, sizeof(hintbuf), "tap to answer on mac");
    lv_label_set_text(s_hint, hintbuf);

    if (r.open_state != OPEN_NONE &&
        strncmp(r.open_id, qs->id, BUDDY_SID_LEN) == 0) {
      if (r.open_state == OPEN_SENDING) {
        lv_label_set_text(s_eyebrow, "opening...");
        lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      } else if (r.open_state == OPEN_OK) {
        lv_label_set_text(s_eyebrow, "opened");
        lv_obj_set_style_text_color(s_eyebrow, t->up, 0);
      } else {
        lv_label_set_text(s_eyebrow, "couldn't open");
        lv_obj_set_style_text_color(s_eyebrow, t->down, 0);
      }
    }

    if (q_count > 1) {
      char badge[16]; snprintf(badge, sizeof(badge), "(1 of %u)", (unsigned)q_count);
      lv_label_set_text(s_q_badge, badge);
      lv_obj_clear_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_q_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    show_prompt(false);
    // session list: up to SESSION_ROWS rows, each with folder (state-colored) + branch+age sub-line.
    uint8_t n = (r.session_count < SESSION_ROWS) ? r.session_count : SESSION_ROWS;
    if (n == 0) {
      lv_obj_add_flag(s_row_btn[0],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[0], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_row_folder[0], "idle");
      lv_obj_set_style_text_color(s_row_folder[0], t->ink_dim, 0);
      lv_label_set_text(s_row_sub[0], "");
      lv_label_set_text(s_row_state[0], "");
      lv_label_set_text(s_row_age[0], "");
      for (uint8_t i = 1; i < SESSION_ROWS; i++) {
        lv_obj_add_flag(s_row_btn[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_row_tick[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_row_folder[i], "");
        lv_label_set_text(s_row_sub[i], "");
        lv_label_set_text(s_row_state[i], "");
        lv_label_set_text(s_row_age[i], "");
      }
    } else {
      for (uint8_t i = 0; i < SESSION_ROWS; i++) {
        if (i < n) {
          const buddy_session_t* s = &r.sessions[i];
          lv_obj_clear_flag(s_row_btn[i], LV_OBJ_FLAG_HIDDEN);

          char folder[BUDDY_LABEL_LEN], branch[BUDDY_LABEL_LEN];
          buddy_session_split_label(s->label, folder, sizeof(folder), branch, sizeof(branch));

          char age[12];
          buddy_session_age(s->ts, age, sizeof(age));

          bool has_branch = branch[0] != '\0';
          lv_color_t tick_col = buddy_session_state_color(t, s->state);
          lv_obj_set_style_bg_color(s_row_tick[i], tick_col, 0);
          lv_obj_set_height(s_row_tick[i], has_branch ? TICK_TALL : TICK_SHORT);
          lv_obj_clear_flag(s_row_tick[i], LV_OBJ_FLAG_HIDDEN);

          lv_label_set_text(s_row_folder[i], folder[0] ? folder : s->id);
          lv_obj_set_style_text_color(s_row_folder[i], t->ink, 0);

          lv_label_set_text(s_row_sub[i], branch);

          if (r.open_state != OPEN_NONE &&
              strncmp(r.open_id, s->id, BUDDY_SID_LEN) == 0) {
            if (r.open_state == OPEN_SENDING) {
              lv_label_set_text(s_row_state[i], "opening...");
              lv_obj_set_style_text_color(s_row_state[i], t->ink_dim, 0);
            } else if (r.open_state == OPEN_OK) {
              lv_label_set_text(s_row_state[i], "opened");
              lv_obj_set_style_text_color(s_row_state[i], t->up, 0);
            } else {
              lv_label_set_text(s_row_state[i], "couldn't");
              lv_obj_set_style_text_color(s_row_state[i], t->down, 0);
            }
          } else {
            // lowercase: use state word as-is
            const char* sw = buddy_session_state_word(s->state);
            lv_label_set_text(s_row_state[i], sw);
            lv_color_t sc;
            if (s->state == BST_ATTENTION)     sc = t->accent;
            else if (s->state == BST_WAITING)  sc = t->down;
            else                               sc = t->ink_dim;
            lv_obj_set_style_text_color(s_row_state[i], sc, 0);
          }

          lv_label_set_text(s_row_age[i], age);
        } else {
          lv_obj_add_flag(s_row_btn[i],  LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(s_row_tick[i], LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(s_row_folder[i], "");
          lv_label_set_text(s_row_sub[i], "");
          lv_label_set_text(s_row_state[i], "");
          lv_label_set_text(s_row_age[i], "");
        }
      }
    }
  }
}

extern const screen_view_t buddy_analog_view = { build, update };
