#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include "core/timekeep.h"
#include "core/records.h"

// Clock + date from the time service (FR-HOME-3). `set` writes text to the label
// (lv_label_set_text or diff-aware txt_set). `date_fmt` is a strftime pattern;
// the result is uppercased. Show "--" until the time service has a fix.
typedef void (*label_setter_t)(lv_obj_t*, const char*);

static inline void render_clock_ex(lv_obj_t* clock_lbl, lv_obj_t* date_lbl,
                                   const char* date_fmt, label_setter_t set) {
  if (!timekeep_has_time()) { set(clock_lbl, "--:--"); set(date_lbl, "--"); return; }
  struct tm lt; timekeep_localtime(&lt);
  char hm[8];  strftime(hm, sizeof(hm), "%H:%M", &lt);  set(clock_lbl, hm);
  char dt[24]; strftime(dt, sizeof(dt), date_fmt, &lt);
  for (char* p = dt; *p; ++p) *p = (char)toupper((unsigned char)*p);
  set(date_lbl, dt);
}

static inline void lv_set(lv_obj_t* l, const char* s) { lv_label_set_text(l, s); }

// Status chip update: when the record is non-live, show the state chip (colored
// down for severe states); otherwise show the battery chip.
// Returns true if the chip is showing a non-live state.
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/batt_chip.h"

static inline bool status_chip_update(lv_obj_t* lbl, const record_hdr_t* hdr,
                                      uint32_t now, const beacon_theme_t* t,
                                      bool caps, const char* bat_prefix,
                                      label_setter_t set) {
  char buf[24];
  if (sv_status(buf, sizeof(buf), hdr, now)) {
    set(lbl, buf);
    lv_obj_set_style_text_color(lbl, sv_severe(hdr->state) ? t->down : t->ink_dim, 0);
    return true;
  }
  char bv[12]; lv_color_t bc = batt_chip(bv, sizeof(bv), caps, t);
  snprintf(buf, sizeof(buf), "%s%s", bat_prefix, bv);
  set(lbl, buf);
  lv_obj_set_style_text_color(lbl, bc, 0);
  return false;
}

// Session-list helpers (Task 10). Used by buddy_calm and future per-theme views.

// State cue: accent for sessions needing attention; down (amber) for waiting; ink for working;
// ink_dim for queued/idle. (One accent editorial rule from DESIGN.md — accent reserved for the
// single session that needs you.)
static inline lv_color_t buddy_session_state_color(const beacon_theme_t* t, uint8_t st) {
  switch (st) {
    case BST_ATTENTION:      return t->accent;
    case BST_WAITING:        return t->down;
    case BST_WORKING:        return t->ink;
    case BST_QUESTION:       return t->accent;
    default:                 return t->ink_dim;  // queued / idle
  }
}
static inline const char* buddy_session_glyph(uint8_t st) {
  switch (st) { case BST_ATTENTION: return "*"; case BST_WAITING: return "!";
                case BST_WAITING_QUEUED: return "."; case BST_WORKING: return ">"; case BST_QUESTION: return "?"; default: return "-"; }
}

// Short state word for the session row's right column. Lowercase; views uppercase per their idiom.
static inline const char* buddy_session_state_word(uint8_t st) {
  switch (st) {
    case BST_ATTENTION:      return "attention";
    case BST_WAITING:        return "waiting";
    case BST_WAITING_QUEUED: return "queued";
    case BST_IDLE:           return "idle";
    case BST_QUESTION:       return "question";
    default:                 return "working";
  }
}

// Relative age from a hub epoch ts using the device's synced wall clock. Empty when clock unsynced
// (design §5: never render a garbage delta) or ts==0.
static inline void buddy_session_age(uint32_t ts, char* out, size_t n) {
  if (!timekeep_has_time() || ts == 0) { out[0] = '\0'; return; }
  time_t now = time(NULL);
  long d = (long)now - (long)ts;
  if (d < 5)         snprintf(out, n, "now");
  else if (d < 60)   snprintf(out, n, "%lds", d);
  else if (d < 3600) snprintf(out, n, "%ldm", d / 60);
  else               snprintf(out, n, "%ldh", d / 3600);
}

// Split the hub label "folder · branch" into its two parts for the row's two lines. The separator
// is the UTF-8 middle dot "\xC2\xB7"; absent => the whole label is folder and branch is empty.
static inline void buddy_session_split_label(const char* label, char* folder, size_t fn,
                                             char* branch, size_t bn) {
  const char* sep = strstr(label ? label : "", "\xC2\xB7");
  if (!sep) { snprintf(folder, fn, "%s", label ? label : ""); branch[0] = '\0'; return; }
  size_t flen = (size_t)(sep - label);
  while (flen > 0 && label[flen - 1] == ' ') flen--;   // trim trailing space before the dot
  snprintf(folder, fn, "%.*s", (int)flen, label);
  const char* b = sep + 2;                              // skip the 2-byte separator
  while (*b == ' ') b++;                                // trim leading space after the dot
  snprintf(branch, bn, "%s", b);
}

// Buddy telemetry stats line: "N RUN . N WAIT". `caps` controls case.
static inline void buddy_stats_fmt(char* buf, size_t n, const buddy_rec_t* b, bool caps) {
  if (caps)
    snprintf(buf, n, "%u RUN . %u WAIT",
             (unsigned)b->running, (unsigned)b->waiting);
  else
    snprintf(buf, n, "%u run . %u wait",
             (unsigned)b->running, (unsigned)b->waiting);
}
