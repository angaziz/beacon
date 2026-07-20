// Calm Futurism CLAUDE (coding buddy) view. Sparse white-on-black. Top: [dot] claude + status
// slot. A dim stat line (running / waiting). prompt.present => big Doto tool name,
// a faint hint box, and DENY | APPROVE actions. Else => session list (up to 4 rows, 64px pitch)
// or "idle" / "no sessions" empty state.
// Decide routes through buddy_decide; the prompt waits on the hub ack and clears only on ok:true,
// else warns "too late" with a dismiss. Actions disabled when hub offline.
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
static void update(void);

#define SESSION_ROWS     4
#define SESSION_ROW_H   64
// First row top-y: below the stat line (SAFE_INSET + 36 + ~28px font clearance).
#define SESSION_LIST_Y  (SAFE_INSET + 72)
// Tick heights: SHORT brackets the folder line; TALL spans down to the branch line (sub y +26
// plus the branch line height).
#define TICK_SHORT      20
#define TICK_TALL       42

static lv_obj_t *s_status, *s_stat;
static lv_obj_t *s_eyebrow, *s_tool, *s_hintbox, *s_hint;
static lv_obj_t *s_deny, *s_approve;
static bool      s_actions_enabled;
static lv_obj_t *s_q_btn, *s_q_badge;
static uint8_t   s_q_session_idx;  // index of displayed question session

// Session rows: each row has a folder label (top) and a branch label (bottom dim),
// plus a tick bar (left) and state word + age (right).
static lv_obj_t *s_row_folder[SESSION_ROWS];
static lv_obj_t *s_row_sub[SESSION_ROWS];   // branch, dim mono
static lv_obj_t *s_row_tick[SESSION_ROWS];    // leading state bar
static lv_obj_t *s_row_state[SESSION_ROWS];   // right-aligned state word
static lv_obj_t *s_row_age[SESSION_ROWS];     // right-aligned age
static lv_obj_t *s_row_btn[SESSION_ROWS];     // transparent tap target (issue #110 Phase 2)

static void on_deny(lv_event_t* e) {
  (void)e;
  if (idle_take_wake_tap()) return;
  if (buddy_dismiss()) { update(); return; }   // clear a "too late" warning
  if (!s_actions_enabled) return;
  if (buddy_decide(false)) update();
}
static void on_approve(lv_event_t* e) {
  (void)e;
  if (idle_take_wake_tap()) return;
  if (!s_actions_enabled) return;
  if (buddy_decide(true)) update();
}
static void on_row_tap(lv_event_t* e) {
  if (idle_take_wake_tap()) return;
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  buddy_rec_t b = ds_get_buddy();
  if (idx < b.session_count) {
    buddy_open(b.sessions[idx].id);
    update();
  }
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

  lv_obj_t* dot = lv_obj_create(page);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(dot, t->accent, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 4, SAFE_INSET + 12);

  lv_obj_t* brand = lv_label_create(page);
  lv_label_set_text(brand, "agents");
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
  lv_obj_set_ext_click_area(s_deny, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_deny, on_deny, LV_EVENT_CLICKED, NULL);

  s_approve = lv_label_create(page);
  lv_label_set_text(s_approve, "approve >");
  lv_obj_set_style_text_font(s_approve, t->f_body, 0);
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_obj_set_style_text_letter_space(s_approve, 2, 0);
  lv_obj_align(s_approve, LV_ALIGN_BOTTOM_RIGHT, -(SAFE_INSET + 4), -SAFE_INSET);
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_approve, BUDDY_HIT_SLOP);
  lv_obj_add_event_cb(s_approve, on_approve, LV_EVENT_CLICKED, NULL);

  // Session rows (shown when no prompt). Two-column layout per row:
  // Left: [tick bar] [folder / branch]. Right: [state word / age].
  // Row pitch: SESSION_ROW_H px. All objects created here; only text/color/visibility mutated in update().
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
    lv_obj_set_user_data(s_row_btn[i], (void*)(uintptr_t)i);
    lv_obj_add_event_cb(s_row_btn[i], on_row_tap, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    // 3px vertical tick bar; top-anchored to the folder line. Height is set per row in update()
    // (TICK_SHORT = folder line only; TICK_TALL = spans down to the branch line).
    s_row_tick[i] = lv_obj_create(page);
    lv_obj_remove_style_all(s_row_tick[i]);
    lv_obj_set_size(s_row_tick[i], 3, TICK_SHORT);
    lv_obj_set_style_bg_opa(s_row_tick[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_row_tick[i], t->ink_dim, 0);
    lv_obj_set_style_radius(s_row_tick[i], 1, 0);

    // Folder: body font, ink color, width leaves room for right meta block (~90px).
    s_row_folder[i] = lv_label_create(page);
    lv_label_set_text(s_row_folder[i], "");
    lv_obj_set_style_text_font(s_row_folder[i], t->f_body, 0);
    lv_obj_set_style_text_color(s_row_folder[i], t->ink, 0);
    lv_obj_set_style_text_letter_space(s_row_folder[i], 1, 0);
    lv_obj_set_width(s_row_folder[i], SCREEN_W - 2 * SAFE_INSET - 12 - 90);
    lv_label_set_long_mode(s_row_folder[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_folder[i], LV_ALIGN_TOP_LEFT, SAFE_INSET + 12, y);
    // Top-align the tick to the folder line; height (folder-only vs. spanning the branch)
    // is set per row in update(). Small +y nudge brackets the folder cap-height cleanly.
    lv_obj_align_to(s_row_tick[i], s_row_folder[i], LV_ALIGN_OUT_LEFT_TOP, -8, 4);

    // Branch (secondary left, dim mono).
    s_row_sub[i] = lv_label_create(page);
    lv_label_set_text(s_row_sub[i], "");
    lv_obj_set_style_text_font(s_row_sub[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_sub[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_sub[i], SCREEN_W - 2 * SAFE_INSET - 12 - 90);
    lv_label_set_long_mode(s_row_sub[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_sub[i], LV_ALIGN_TOP_LEFT, SAFE_INSET + 12, y + 26);

    // State word (right, top) — lowercase for calm theme.
    s_row_state[i] = lv_label_create(page);
    lv_label_set_text(s_row_state[i], "");
    lv_obj_set_style_text_font(s_row_state[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_state[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_state[i], 88);
    lv_label_set_long_mode(s_row_state[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_state[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, y);

    // Age (right, below state word) — mono, dim.
    s_row_age[i] = lv_label_create(page);
    lv_label_set_text(s_row_age[i], "");
    lv_obj_set_style_text_font(s_row_age[i], t->f_mono, 0);
    lv_obj_set_style_text_color(s_row_age[i], t->ink_dim, 0);
    lv_obj_set_width(s_row_age[i], 88);
    lv_label_set_long_mode(s_row_age[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_row_age[i], LV_ALIGN_TOP_RIGHT, -SAFE_INSET, y + 26);
  }

  // Question card: full-page transparent tap target (hidden by default)
  s_q_btn = lv_btn_create(page);
  lv_obj_remove_style_all(s_q_btn);
  lv_obj_set_size(s_q_btn, lv_pct(100), lv_pct(100));
  lv_obj_align(s_q_btn, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(s_q_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_q_btn, on_question_tap, LV_EVENT_CLICKED, NULL);

  // Badge: "(1 of N)" — shown only when multiple question sessions
  s_q_badge = lv_label_create(page);
  lv_obj_set_style_text_font(s_q_badge, t->f_mono, 0);
  lv_obj_set_style_text_color(s_q_badge, t->ink_dim, 0);
  lv_label_set_text(s_q_badge, "");
  lv_obj_align(s_q_badge, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET + 52);
  lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);

  update();
}

static void show_prompt(bool on) {
  lv_obj_t* p[] = { s_eyebrow, s_tool, s_hintbox, s_deny, s_approve };
  for (lv_obj_t* o : p) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  for (uint8_t i = 0; i < SESSION_ROWS; i++) {
    if (on) {
      lv_obj_add_flag(s_row_btn[i],     LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_folder[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_sub[i],     LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_state[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_age[i],     LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_row_btn[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_tick[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }
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
    snprintf(st, sizeof(st), "%u run . %u wait",
             (unsigned)b.running, (unsigned)b.waiting);
    lv_label_set_text(s_stat, st);
  }

  // Find newest question session (sessions are hub-sorted newest-first)
  int8_t q_idx = -1;
  uint8_t q_count = 0;
  for (uint8_t i = 0; i < b.session_count; i++) {
    if (b.sessions[i].state == BST_QUESTION) {
      if (q_idx < 0) q_idx = (int8_t)i;  // first = newest
      q_count++;
    }
  }

  if (b.prompt.present) {
    // TIER 1: PROMPT MODE
    lv_obj_add_flag(s_q_btn,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
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
      char badge[16]; buddy_queue_badge(b.prompt.queue_len, badge, sizeof(badge));
      char eb[48];
      snprintf(eb, sizeof(eb), "approve?%s %us",
               badge, (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      lv_label_set_text(s_eyebrow, eb);
      lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      lv_label_set_text(s_deny, "< deny");
      // Dim actions visually when disabled (hub offline).
      lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
      lv_obj_set_style_text_color(s_approve, s_actions_enabled ? t->accent : t->ink_dim, 0);
      break;
    }
    }
  } else if (q_idx >= 0) {
    // TIER 2: QUESTION CARD
    s_q_session_idx = (uint8_t)q_idx;
    const buddy_session_t* qs = &b.sessions[q_idx];

    // Hide session list objects
    for (int i = 0; i < SESSION_ROWS; i++) {
      lv_obj_add_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }
    // Hide prompt-only actions; show shared prompt layout objects
    lv_obj_add_flag(s_deny,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_approve, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_eyebrow, LV_OBJ_FLAG_HIDDEN);

    // Eyebrow: "your turn" (overridden below if open feedback is active)
    lv_label_set_text(s_eyebrow, "your turn");
    lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);

    // Folder (prominent) in s_tool; branch in s_hint inside s_hintbox
    char folder[BUDDY_LABEL_LEN], branch[BUDDY_LABEL_LEN];
    buddy_session_split_label(qs->label, folder, sizeof(folder), branch, sizeof(branch));
    lv_label_set_text(s_tool, folder[0] ? folder : qs->id);
    lv_obj_clear_flag(s_tool, LV_OBJ_FLAG_HIDDEN);

    // Branch dim + hint in hintbox
    char hintbuf[BUDDY_HINT_LEN + 32];
    if (branch[0])
      snprintf(hintbuf, sizeof(hintbuf), "%s\ntap to answer on mac", branch);
    else
      snprintf(hintbuf, sizeof(hintbuf), "tap to answer on mac");
    lv_label_set_text(s_hint, hintbuf);
    lv_obj_clear_flag(s_hintbox, LV_OBJ_FLAG_HIDDEN);

    // Open feedback: if this question session has open feedback, show on eyebrow
    if (b.open_state != OPEN_NONE &&
        strncmp(b.open_id, qs->id, BUDDY_SID_LEN) == 0) {
      if (b.open_state == OPEN_SENDING) {
        lv_label_set_text(s_eyebrow, "opening...");
        lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
      } else if (b.open_state == OPEN_OK) {
        lv_label_set_text(s_eyebrow, "opened");
        lv_obj_set_style_text_color(s_eyebrow, t->up, 0);
      } else {
        lv_label_set_text(s_eyebrow, "couldn't open");
        lv_obj_set_style_text_color(s_eyebrow, t->down, 0);
      }
    }

    // Badge: "(1 of N)" when multiple question sessions
    if (q_count > 1) {
      char badge[16];
      snprintf(badge, sizeof(badge), "(1 of %u)", (unsigned)q_count);
      lv_label_set_text(s_q_badge, badge);
      lv_obj_clear_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(s_q_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    // TIER 3: SESSION LIST
    lv_obj_add_flag(s_q_btn,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_q_badge, LV_OBJ_FLAG_HIDDEN);
    show_prompt(false);

    uint8_t n = (b.session_count < SESSION_ROWS) ? b.session_count : SESSION_ROWS;

    if (n == 0) {
      // Empty state: "idle" on folder, no tick/meta, others blank.
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
          const buddy_session_t* s = &b.sessions[i];
          lv_obj_clear_flag(s_row_btn[i], LV_OBJ_FLAG_HIDDEN);

          char folder[BUDDY_LABEL_LEN], branch[BUDDY_LABEL_LEN];
          buddy_session_split_label(s->label, folder, sizeof(folder), branch, sizeof(branch));

          char age[12];
          buddy_session_age(s->ts, age, sizeof(age));

          // Tick: colored by state; spans both lines when a branch is present.
          bool has_branch = branch[0] != '\0';
          lv_color_t tick_col = buddy_session_state_color(t, s->state);
          lv_obj_set_style_bg_color(s_row_tick[i], tick_col, 0);
          lv_obj_set_height(s_row_tick[i], has_branch ? TICK_TALL : TICK_SHORT);
          lv_obj_clear_flag(s_row_tick[i], LV_OBJ_FLAG_HIDDEN);

          // Folder: ink, no glyph prefix.
          lv_label_set_text(s_row_folder[i], folder[0] ? folder : s->id);
          lv_obj_set_style_text_color(s_row_folder[i], t->ink, 0);

          // Branch (sub-line): dim mono.
          lv_label_set_text(s_row_sub[i], branch);

          // State word: override with open feedback if this row's session is the open target.
          if (b.open_state != OPEN_NONE &&
              strncmp(b.open_id, s->id, BUDDY_SID_LEN) == 0) {
            if (b.open_state == OPEN_SENDING) {
              lv_label_set_text(s_row_state[i], "opening...");
              lv_obj_set_style_text_color(s_row_state[i], t->ink_dim, 0);
            } else if (b.open_state == OPEN_OK) {
              lv_label_set_text(s_row_state[i], "opened");
              lv_obj_set_style_text_color(s_row_state[i], t->up, 0);
            } else {
              lv_label_set_text(s_row_state[i], "couldn't");
              lv_obj_set_style_text_color(s_row_state[i], t->down, 0);
            }
          } else {
            // Normal state word: accent for ATTENTION, down for WAITING, else ink_dim.
            const char* sw = buddy_session_state_word(s->state);
            lv_label_set_text(s_row_state[i], sw);
            lv_color_t sc;
            if (s->state == BST_ATTENTION)     sc = t->accent;
            else if (s->state == BST_WAITING)  sc = t->down;
            else                               sc = t->ink_dim;
            lv_obj_set_style_text_color(s_row_state[i], sc, 0);
          }

          // Age: dim mono (empty when clock unsynced).
          lv_label_set_text(s_row_age[i], age);
        } else {
          // Unused rows: hide tick + blank all labels.
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

extern const screen_view_t buddy_calm_view = { build, update };
