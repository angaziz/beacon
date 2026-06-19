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

// Buddy telemetry stats line: "N RUN . N WAIT . NK TOK . CTX N%".
// `caps` controls case.
static inline void buddy_stats_fmt(char* buf, size_t n, const buddy_rec_t* b, bool caps) {
  if (caps)
    snprintf(buf, n, "%u RUN . %u WAIT . %uK TOK . CTX %u%%",
             (unsigned)b->running, (unsigned)b->waiting,
             (unsigned)(b->tokens / 1000), (unsigned)b->context_pct);
  else
    snprintf(buf, n, "%u run . %u wait . %uk tok . ctx %u%%",
             (unsigned)b->running, (unsigned)b->waiting,
             (unsigned)(b->tokens / 1000), (unsigned)b->context_pct);
}
