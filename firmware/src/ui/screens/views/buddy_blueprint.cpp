// Blueprint / Schematic - CLAUDE (coding buddy). Technical DWG header + status dimension
// line. Prompt present => tool figure + hint box (schematic frame) + DENY|APPROVE; else idle
// => recent entries or "idle". Approve/Deny is a LOCAL STUB. Grid/reticle drawn by chrome.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/hub_task.h"
#include "util/log.h"
#include <Arduino.h>

static lv_obj_t *s_status, *s_stat;          // header chip + status dimension line
static lv_obj_t *s_eyebrow, *s_tool, *s_hintbox, *s_hint;
static lv_obj_t *s_deny, *s_approve, *s_actrule;
static lv_obj_t *s_idle;                      // idle entries label

static bool actions_locked(screen_state_t st) {
  return st == ST_HUB_OFFLINE || st == ST_RECONNECTING;
}

static void decide_cb(lv_event_t* e) {
  bool approve = (bool)(intptr_t)lv_event_get_user_data(e);
  buddy_rec_t r = ds_get_buddy();
  if (actions_locked(r.hdr.state) || !r.prompt.present) return;
  if (!hub_send_permission(r.prompt.id, approve)) return;   // keep prompt visible if not enqueued
  r.prompt.present = false;
  ds_set_buddy(&r);
  LOGI("buddy %s prompt %s", approve ? "APPROVE" : "DENY", r.prompt.id);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* dwg = lv_label_create(page);
  lv_label_set_text(dwg, "DWG. BEACON-004 / CLAUDE");
  lv_obj_set_style_text_color(dwg, t->ink_dim, 0);
  lv_obj_set_style_text_font(dwg, t->f_mono, 0);
  lv_obj_align(dwg, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "REQ ----");
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  // Status dimension line: running/waiting/tokens/context.
  s_stat = lv_label_create(page);
  lv_label_set_text(s_stat, "- RUN . - WAIT . --K TOK . CTX --%");
  lv_obj_set_style_text_color(s_stat, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_stat, t->f_mono, 0);
  lv_obj_align(s_stat, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 26);

  // Eyebrow (prompt prompt). PERMISSION - APPROVE?
  s_eyebrow = lv_label_create(page);
  lv_label_set_text(s_eyebrow, "PERMISSION - APPROVE?");
  lv_obj_set_style_text_color(s_eyebrow, t->accent, 0);
  lv_obj_set_style_text_font(s_eyebrow, t->f_mono, 0);
  lv_obj_align(s_eyebrow, LV_ALIGN_LEFT_MID, SAFE_INSET, -64);

  // Tool figure.
  s_tool = lv_label_create(page);
  lv_label_set_text(s_tool, "--");
  lv_obj_set_style_text_color(s_tool, t->ink, 0);
  lv_obj_set_style_text_font(s_tool, t->f_display, 0);
  lv_obj_align(s_tool, LV_ALIGN_LEFT_MID, SAFE_INSET, -20);

  // Hint box: schematic frame (hairline border, transparent fill).
  s_hintbox = lv_obj_create(page);
  lv_obj_remove_style_all(s_hintbox);
  lv_obj_set_size(s_hintbox, SCREEN_W - 2 * SAFE_INSET, 56);
  lv_obj_align(s_hintbox, LV_ALIGN_LEFT_MID, SAFE_INSET, 36);
  lv_obj_clear_flag(s_hintbox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(s_hintbox, t->stroke_hair, 0);
  lv_obj_set_style_border_color(s_hintbox, t->line, 0);
  lv_obj_set_style_pad_left(s_hintbox, SPACE_M, 0);

  s_hint = lv_label_create(s_hintbox);
  lv_label_set_text(s_hint, "--");
  lv_obj_set_style_text_color(s_hint, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_hint, t->f_mono, 0);
  lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 0, 0);

  // Action rule + DENY | APPROVE buttons (labels with click events).
  s_actrule = lv_obj_create(page);
  lv_obj_remove_style_all(s_actrule);
  lv_obj_set_size(s_actrule, SCREEN_W - 2 * SAFE_INSET, t->stroke_hair);
  lv_obj_align(s_actrule, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 30));
  lv_obj_set_style_bg_color(s_actrule, t->line, 0);
  lv_obj_set_style_bg_opa(s_actrule, LV_OPA_COVER, 0);

  s_deny = lv_label_create(page);
  lv_label_set_text(s_deny, "< DENY");
  lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_deny, t->f_mono, 0);
  lv_obj_align(s_deny, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_deny, decide_cb, LV_EVENT_CLICKED, (void*)(intptr_t)false);

  s_approve = lv_label_create(page);
  lv_label_set_text(s_approve, "APPROVE >");
  lv_obj_set_style_text_color(s_approve, t->accent, 0);
  lv_obj_set_style_text_font(s_approve, t->f_mono, 0);
  lv_obj_align(s_approve, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
  lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_approve, decide_cb, LV_EVENT_CLICKED, (void*)(intptr_t)true);

  // Idle layout (recent entries / "idle"), hidden when a prompt is present.
  s_idle = lv_label_create(page);
  lv_label_set_text(s_idle, "idle");
  lv_obj_set_style_text_color(s_idle, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_idle, t->f_mono, 0);
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
  const beacon_theme_t* t = theme_active();
  buddy_rec_t r = ds_get_buddy();
  uint32_t now = now_s();

  char buf[32];
  if (sv_status(buf, sizeof(buf), &r.hdr, now)) lv_label_set_text(s_status, buf);
  else {
    char rb[16]; snprintf(rb, sizeof(rb), "REQ %s", r.prompt.present ? r.prompt.id : "----");
    lv_label_set_text(s_status, rb);
  }

  bool ph = sv_placeholder(r.hdr.state);
  char sb[64];
  if (ph) snprintf(sb, sizeof(sb), "- RUN . - WAIT . --K TOK . CTX --%%");
  else snprintf(sb, sizeof(sb), "%u RUN . %u WAIT . %uK TOK . CTX %u%%",
                (unsigned)r.running, (unsigned)r.waiting,
                (unsigned)(r.tokens / 1000), (unsigned)r.context_pct);
  lv_label_set_text(s_stat, sb);

  show_prompt(r.prompt.present);

  if (r.prompt.present) {
    lv_label_set_text(s_tool, r.prompt.tool);
    lv_label_set_text(s_hint, r.prompt.hint);
    bool locked = actions_locked(r.hdr.state);
    lv_obj_set_style_text_color(s_deny, t->ink_dim, 0);
    lv_obj_set_style_text_color(s_approve, locked ? t->ink_dim : t->accent, 0);
    if (locked) {
      lv_obj_clear_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
    } else {
      lv_obj_add_flag(s_deny, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(s_approve, LV_OBJ_FLAG_CLICKABLE);
    }
  } else {
    if (r.entry_count > 0) lv_label_set_text(s_idle, r.entries[0]);
    else lv_label_set_text(s_idle, "idle");
  }
}

extern const screen_view_t buddy_blueprint_view = { build, update };
