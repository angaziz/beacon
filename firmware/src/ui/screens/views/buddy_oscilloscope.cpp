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
#include <string.h>

// Oscilloscope / Signal CLAUDE. Scope-instrument language: status telemetry line, then either
// a permission PROMPT (tool name as the signal, hint in a bordered scope box, DENY|APPROVE
// readouts) or idle session list. Decide routes through buddy_decide; the prompt waits on the hub
// ack and clears only on ok:true, else warns "too late" with a dismiss. Actions disabled on HUB_OFFLINE.

#define SESSION_ROWS    4
#define SESSION_ROW_H   56
#define SESSION_LIST_Y  (SAFE_INSET + 100)
// Tick heights: SHORT brackets the folder line; TALL spans down to the branch line.
#define TICK_SHORT      16
#define TICK_TALL       38

static lv_obj_t* s_telem;
static lv_obj_t* s_eyebrow;     // "PERMISSION - APPROVE?" / "ACTIVITY"
static lv_obj_t* s_tool;        // big tool name (prompt mode only)
static lv_obj_t* s_box;         // hint scope box
static lv_obj_t* s_hint;        // hint text
static lv_obj_t* s_deny;
static lv_obj_t* s_approve;

static lv_obj_t* s_row_folder[SESSION_ROWS];
static lv_obj_t* s_row_sub[SESSION_ROWS];
static lv_obj_t* s_row_tick[SESSION_ROWS];    // leading state bar
static lv_obj_t* s_row_state[SESSION_ROWS];   // right-aligned state word
static lv_obj_t* s_row_age[SESSION_ROWS];     // right-aligned age
static lv_obj_t* s_row_btn[SESSION_ROWS];     // transparent tap target (issue #110 Phase 2)

static void update(void);

static void decide_cb(lv_event_t* e) {
  if (idle_take_wake_tap()) return;
  bool approve = (bool)(intptr_t)lv_event_get_user_data(e);
  if (!approve && buddy_dismiss()) { update(); return; }   // deny doubles as dismiss for "too late"
  if (buddy_decide(approve)) update();
}
static void on_row_tap(lv_event_t* e) {
  if (idle_take_wake_tap()) return;
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  buddy_rec_t b = ds_get_buddy();
  if (idx < b.session_count) { buddy_open(b.sessions[idx].id); update(); }
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

  // Session rows (shown in idle mode below the ACTIVITY eyebrow).
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
    // Show prompt objects, hide session rows.
    lv_obj_clear_flag(s_tool, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < SESSION_ROWS; i++) {
      lv_obj_add_flag(s_row_btn[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_tick[i],   LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }

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
      char badge[16]; buddy_queue_badge(b.prompt.queue_len, badge, sizeof(badge));
      char eb[48];
      snprintf(eb, sizeof(eb), "PERMISSION - APPROVE?%s %us",
               badge, (unsigned)buddy_prompt_secs_left(&b, uptime_s()));
      lv_label_set_text(s_eyebrow, eb);
      lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);
      lv_label_set_text(s_deny, "< DENY");
      show_actions(true, !offline, t);
      break;
    }
    }
  } else {
    lv_label_set_text(s_eyebrow, "ACTIVITY");
    lv_obj_set_style_text_color(s_eyebrow, t->ink_dim, 0);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tool, LV_OBJ_FLAG_HIDDEN);  // tool label is prompt-only
    show_actions(false, false, t);

    for (uint8_t i = 0; i < SESSION_ROWS; i++) {
      lv_obj_clear_flag(s_row_folder[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_sub[i],    LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_state[i],  LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_row_age[i],    LV_OBJ_FLAG_HIDDEN);
    }

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
    } else {
      for (uint8_t i = 0; i < SESSION_ROWS; i++) {
        if (i < n) {
          const buddy_session_t* s = &b.sessions[i];
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

extern const screen_view_t buddy_oscilloscope_view = { build, update };
