#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "ui/screens/views/view_common.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include "ui/idle_glue.h"
#include <Arduino.h>
#include <string.h>

// LED Matrix / CLAUDE: status line + permission prompt (tool lit, hint in a box, DENY|APPROVE)
// or idle session list. Decide routes through buddy_decide; the prompt waits on the hub ack
// ("SENT -- AWAITING") and clears only on ok:true, else warns "TOO LATE" with a dismiss.

#define SESSION_ROWS    4
#define SESSION_ROW_H   58
#define SESSION_LIST_Y  (SAFE_INSET + 70)
// Tick heights: SHORT brackets the folder line; TALL spans down to the branch line.
#define TICK_SHORT      16
#define TICK_TALL       38

static lv_obj_t *s_status, *s_stats;
static lv_obj_t *s_prompt_box, *s_prompt_lead, *s_tool, *s_hint;
static lv_obj_t *s_actions, *s_deny, *s_approve;
static lv_obj_t *s_row_folder[SESSION_ROWS];
static lv_obj_t *s_row_sub[SESSION_ROWS];
static lv_obj_t *s_row_tick[SESSION_ROWS];    // leading state bar
static lv_obj_t *s_row_state[SESSION_ROWS];   // right-aligned state word
static lv_obj_t *s_row_age[SESSION_ROWS];     // right-aligned age
static lv_obj_t *s_row_btn[SESSION_ROWS];     // transparent tap target (issue #110 Phase 2)
static lv_obj_t *s_q_btn, *s_q_badge;
static uint8_t   s_q_session_idx;

static void deny_cb(lv_event_t*)    { if (idle_take_wake_tap()) return; if (!buddy_dismiss()) buddy_decide(false); }
static void approve_cb(lv_event_t*) { if (idle_take_wake_tap()) return; buddy_decide(true); }
static void update(void);
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
static void on_row_tap(lv_event_t* e) {
  if (idle_take_wake_tap()) return;
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  buddy_rec_t b = ds_get_buddy();
  if (idx < b.session_count) buddy_open(b.sessions[idx].id);
}

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

  // Session rows (shown when no prompt).
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
    lv_obj_align_to(s_row_tick[i], s_row_folder[i], LV_ALIGN_OUT_LEFT_TOP, -9, 3);

    s_row_sub[i] = lv_label_create(page);
    lv_label_set_text(s_row_sub[i], "");
    lv_obj_set_style_text_font(s_row_sub[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_sub[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_sub[i], SCREEN_W - 2 * SAFE_INSET - 12 - 90);
    lv_label_set_long_mode(s_row_sub[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_sub[i], LV_ALIGN_TOP_LEFT, SAFE_INSET + 12, y + 22);

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
    lv_obj_align(s_row_age[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, y + 22);
  }

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
  snprintf(line, sizeof(line), "%uR %uW",
           (unsigned)b.running, (unsigned)b.waiting);
  lv_label_set_text(s_stats, line);

  // Find newest question session (first one with BST_QUESTION).
  int8_t q_idx = -1; uint8_t q_count = 0;
  for (uint8_t i = 0; i < b.session_count; i++) {
    if (b.sessions[i].state == BST_QUESTION) {
      if (q_idx < 0) q_idx = (int8_t)i;
      q_count++;
    }
  }

  // Always reset q overlays; each tier sets them explicitly below.
  lv_obj_add_flag(s_q_btn,   LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);

  if (b.prompt.present) {
    // TIER 1: PERMISSION PROMPT — show both boxes, hide session rows.
    show(s_prompt_box, true);
    show(s_actions, true);
    for (uint8_t i = 0; i < SESSION_ROWS; i++) {
      lv_obj_add_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }
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
      char badge[16]; buddy_queue_badge(b.prompt.queue_len, badge, sizeof(badge));
      char eb[48];
      snprintf(eb, sizeof(eb), "PERMISSION%s %us",
               badge, (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      lv_label_set_text(s_prompt_lead, eb);
      lv_obj_set_style_text_color(s_prompt_lead, t->accent, 0);
      lv_label_set_text(s_deny, "< DENY");
      lv_obj_set_style_text_color(s_approve, locked ? t->ink_dim : t->accent, 0);
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);  // deny stays dim either way
      break;
    }
    }
  } else if (q_idx >= 0) {
    // TIER 2: QUESTION CARD — reuse prompt layout, hide deny/approve.
    s_q_session_idx = (uint8_t)q_idx;
    const buddy_session_t* qs = &b.sessions[q_idx];

    show(s_prompt_box, true);
    show(s_actions, false);
    for (uint8_t i = 0; i < SESSION_ROWS; i++) {
      lv_obj_add_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_prompt_lead, "YOUR TURN");
    lv_obj_set_style_text_color(s_prompt_lead, t->accent, 0);

    char folder[BUDDY_LABEL_LEN], branch[BUDDY_LABEL_LEN];
    buddy_session_split_label(qs->label, folder, sizeof(folder), branch, sizeof(branch));
    lv_label_set_text(s_tool, folder[0] ? folder : qs->id);

    char hintbuf[BUDDY_HINT_LEN + 32];
    if (branch[0])
      snprintf(hintbuf, sizeof(hintbuf), "%s\nTAP TO ANSWER ON MAC", branch);
    else
      snprintf(hintbuf, sizeof(hintbuf), "TAP TO ANSWER ON MAC");
    lv_label_set_text(s_hint, hintbuf);

    // Open feedback overrides the eyebrow when this question is being opened.
    if (b.open_state != OPEN_NONE &&
        strncmp(b.open_id, qs->id, BUDDY_SID_LEN) == 0) {
      if (b.open_state == OPEN_SENDING) {
        lv_label_set_text(s_prompt_lead, "OPENING...");
        lv_obj_set_style_text_color(s_prompt_lead, t->ink_dim, 0);
      } else if (b.open_state == OPEN_OK) {
        lv_label_set_text(s_prompt_lead, "OPENED");
        lv_obj_set_style_text_color(s_prompt_lead, t->up, 0);
      } else {
        lv_label_set_text(s_prompt_lead, "COULDN'T OPEN");
        lv_obj_set_style_text_color(s_prompt_lead, t->down, 0);
      }
    }

    if (q_count > 1) {
      char qbadge[16];
      snprintf(qbadge, sizeof(qbadge), "(1 OF %u)", (unsigned)q_count);
      lv_label_set_text(s_q_badge, qbadge);
      lv_obj_clear_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_q_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    // TIER 3: SESSION LIST — hide prompt layout, show rows.
    show(s_prompt_box, false);
    show(s_actions, false);

    uint8_t n = (b.session_count < SESSION_ROWS) ? b.session_count : SESSION_ROWS;
    if (n == 0) {
      lv_obj_add_flag(s_row_btn[0],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[0], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_row_folder[0], "IDLE");
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
      lv_obj_clear_flag(s_row_folder[0], LV_OBJ_FLAG_HIDDEN);
    } else {
      for (uint8_t i = 0; i < SESSION_ROWS; i++) {
        if (i < n) {
          const buddy_session_t* s = &b.sessions[i];
          lv_obj_clear_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);

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

          if (b.open_state != OPEN_NONE &&
              strncmp(b.open_id, s->id, BUDDY_SID_LEN) == 0) {
            if (b.open_state == OPEN_SENDING) {
              lv_label_set_text(s_row_state[i], "OPENING...");
              lv_obj_set_style_text_color(s_row_state[i], t->ink_dim, 0);
            } else if (b.open_state == OPEN_OK) {
              lv_label_set_text(s_row_state[i], "OPENED");
              lv_obj_set_style_text_color(s_row_state[i], t->up, 0);
            } else {
              lv_label_set_text(s_row_state[i], "COULDN'T");
              lv_obj_set_style_text_color(s_row_state[i], t->down, 0);
            }
          } else {
            const char* sw = buddy_session_state_word(s->state);
            char sw_up[16]; size_t k=0;
            for(;sw[k]&&k<sizeof(sw_up)-1;k++) sw_up[k]=(char)toupper((unsigned char)sw[k]);
            sw_up[k]='\0';
            lv_label_set_text(s_row_state[i], sw_up);
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

extern const screen_view_t buddy_led_view = { build, update };
