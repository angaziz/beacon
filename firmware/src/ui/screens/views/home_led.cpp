#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/batt_chip.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/timekeep.h"
#include <Arduino.h>
#include <time.h>
#include <ctype.h>

// LED Matrix / HOME: lit amber dot-panel. Caps BEACON / BAT header, big lit clock,
// weather temp + humidity centered. The amber dot grid bg is drawn by the carousel chrome.

static lv_obj_t *s_status, *s_clock, *s_date, *s_temp, *s_humid;


// Clock + date from the time service. RTC time is always "live" (FR-HOME-3); show "--" until known.
static void render_clock(lv_obj_t* clock, lv_obj_t* date) {
  if (!timekeep_has_time()) { lv_label_set_text(clock, "--:--"); lv_label_set_text(date, "--"); return; }
  struct tm lt; timekeep_localtime(&lt);
  char hm[8];  strftime(hm, sizeof(hm), "%H:%M", &lt);            lv_label_set_text(clock, hm);
  char dt[24]; strftime(dt, sizeof(dt), "%a %d %b", &lt);
  for (char* p = dt; *p; ++p) *p = (char)toupper((unsigned char)*p);
  lv_label_set_text(date, dt);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;

  lv_obj_t* eb = lv_label_create(page);
  lv_label_set_text(eb, "BEACON");
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->ink_dim, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "BAT --");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_clock = lv_label_create(page);
  lv_label_set_text(s_clock, "--:--");
  lv_obj_set_style_text_font(s_clock, t->f_hero, 0);
  lv_obj_set_style_text_color(s_clock, t->accent, 0);
  lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -40);

  s_date = lv_label_create(page);
  lv_label_set_text(s_date, "--");
  lv_obj_set_style_text_font(s_date, t->f_mono, 0);
  lv_obj_set_style_text_color(s_date, t->ink_dim, 0);
  lv_obj_align_to(s_date, s_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, SPACE_M);

  // Weather row: two centered readouts (temp, humidity), big lit figure + caps label.
  lv_obj_t* wx = lv_obj_create(page);
  lv_obj_remove_style_all(wx);
  lv_obj_set_size(wx, SCREEN_W - 2 * SAFE_INSET, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(wx, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(wx, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_align(wx, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET - SPACE_S);

  lv_obj_t* tcol = lv_obj_create(wx);
  lv_obj_remove_style_all(tcol);
  lv_obj_set_size(tcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tcol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s_temp = lv_label_create(tcol);
  lv_label_set_text(s_temp, "--");
  lv_obj_set_style_text_font(s_temp, t->f_display, 0);
  lv_obj_set_style_text_color(s_temp, t->accent, 0);
  lv_obj_t* tl = lv_label_create(tcol);
  lv_label_set_text(tl, "TEMP");
  lv_obj_set_style_text_font(tl, t->f_mono, 0);
  lv_obj_set_style_text_color(tl, t->ink_dim, 0);

  lv_obj_t* hcol = lv_obj_create(wx);
  lv_obj_remove_style_all(hcol);
  lv_obj_set_size(hcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hcol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(hcol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s_humid = lv_label_create(hcol);
  lv_label_set_text(s_humid, "--");
  lv_obj_set_style_text_font(s_humid, t->f_display, 0);
  lv_obj_set_style_text_color(s_humid, t->accent, 0);
  lv_obj_t* hl = lv_label_create(hcol);
  lv_label_set_text(hl, "HUMID");
  lv_obj_set_style_text_font(hl, t->f_mono, 0);
  lv_obj_set_style_text_color(hl, t->ink_dim, 0);
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  if (!t) return;
  render_clock(s_clock, s_date);
  weather_rec_t w = ds_get_weather();
  uint32_t now = now_s();

  char st[24];
  if (sv_status(st, sizeof(st), &w.hdr, now)) {
    lv_label_set_text(s_status, st);
    lv_obj_set_style_text_color(s_status, sv_severe(w.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    char bv[12]; lv_color_t bc = batt_chip(bv, sizeof(bv), true, t);
    char out[20]; snprintf(out, sizeof(out), "BAT %s", bv);
    lv_label_set_text(s_status, out);
    lv_obj_set_style_text_color(s_status, bc, 0);
  }

  lv_color_t vc = sv_dim(w.hdr.state) ? t->ink_dim : t->accent;
  if (sv_placeholder(w.hdr.state)) {
    lv_label_set_text(s_temp, "--");
    lv_label_set_text(s_humid, "--");
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", w.temp_c);
    lv_label_set_text(s_temp, buf);
    snprintf(buf, sizeof(buf), "%.0f%%", w.humidity_pct);
    lv_label_set_text(s_humid, buf);
  }
  lv_obj_set_style_text_color(s_temp, vc, 0);
  lv_obj_set_style_text_color(s_humid, vc, 0);
}

extern const screen_view_t home_led_view = { build, update };
