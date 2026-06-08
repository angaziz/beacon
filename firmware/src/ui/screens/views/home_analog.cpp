#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include "core/timekeep.h"
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

// Analog Neo home: a drawn analog clock face (tick ring + hands + hub) with small digital
// temp/humidity readouts below. Hands track the time service when available, falling back to a
// pleasant ~10:08 position until the clock is known. Readouts + the date chip update from update().


#define DIAL_SIZE 240          // clock face box (well inside SAFE_INSET on 466)
#define HAND_H_LEN 62          // hour hand length (px from center)
#define HAND_M_LEN 92          // minute hand length
#define HAND_S_LEN 100         // second hand length

// Static pleasant hand angles (degrees clockwise from 12 o'clock), ~10:08:36.
// hour 10:08 => 10*30 + 8*0.5 = 304 ; minute 08 => 8*6 = 48 ; second 36 => 36*6 = 216.
#define ANG_HOUR   304
#define ANG_MIN    48
#define ANG_SEC    216

static lv_obj_t *s_dial, *s_temp, *s_hum, *s_status;

// Live date for the top-right chip; lowercase to match this lane's mono eyebrow style.
// Falls back to "--" until the time service has a fix (FR-HOME-3).
static void render_date(lv_obj_t* date) {
  if (!timekeep_has_time()) { lv_label_set_text(date, "--"); return; }
  struct tm lt; timekeep_localtime(&lt);
  char dt[24]; strftime(dt, sizeof(dt), "%a %d %b", &lt);
  for (char* p = dt; *p; ++p) *p = (char)tolower((unsigned char)*p);
  lv_label_set_text(date, dt);
}

// Endpoint of a hand from center (cx,cy), given angle from 12 o'clock and length.
static void hand_end(int cx, int cy, int deg, int len, lv_point_t* out) {
  double rad = (deg - 90) * (M_PI / 180.0);  // -90 so 0deg points up (12 o'clock)
  out->x = (lv_coord_t)(cx + cos(rad) * len);
  out->y = (lv_coord_t)(cy + sin(rad) * len);
}

static void draw_face(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_area_t a; lv_obj_get_coords(o, &a);
  const beacon_theme_t* t = theme_active(); if (!t) return;

  int cx = (a.x1 + a.x2) / 2;
  int cy = (a.y1 + a.y2) / 2;
  int r  = (a.x2 - a.x1) / 2;

  // Live hand angles from the time service; fall back to the pleasant static pose until known.
  int ang_hour = ANG_HOUR, ang_min = ANG_MIN, ang_sec = ANG_SEC;
  if (timekeep_has_time()) {
    struct tm lt; timekeep_localtime(&lt);
    ang_hour = (lt.tm_hour % 12) * 30 + lt.tm_min / 2;
    ang_min  = lt.tm_min * 6;
    ang_sec  = lt.tm_sec * 6;
  }

  // Tick ring: 60 minute ticks (short, dim), with bolder hour ticks every 5th.
  lv_draw_line_dsc_t td; lv_draw_line_dsc_init(&td);
  td.opa = LV_OPA_COVER; td.round_start = 1; td.round_end = 1;
  for (int i = 0; i < 60; i++) {
    bool hour = (i % 5) == 0;
    int deg = i * 6;
    double rad = (deg - 90) * (M_PI / 180.0);
    int outer = r;
    int inner = r - (hour ? 12 : 6);
    td.color = hour ? t->ink_dim : t->line;
    td.width = hour ? t->stroke_med : t->stroke_hair;
    lv_point_t p1 = { (lv_coord_t)(cx + cos(rad) * outer), (lv_coord_t)(cy + sin(rad) * outer) };
    lv_point_t p2 = { (lv_coord_t)(cx + cos(rad) * inner), (lv_coord_t)(cy + sin(rad) * inner) };
    lv_draw_line(ctx, &td, &p1, &p2);
  }

  lv_point_t center = { (lv_coord_t)cx, (lv_coord_t)cy };

  // Hour hand (ink, thick).
  lv_draw_line_dsc_t hd; lv_draw_line_dsc_init(&hd);
  hd.opa = LV_OPA_COVER; hd.round_start = 1; hd.round_end = 1;
  hd.color = t->ink; hd.width = 6;
  lv_point_t he; hand_end(cx, cy, ang_hour, HAND_H_LEN, &he);
  lv_point_t hpts[2] = { center, he };
  lv_draw_line(ctx, &hd, &hpts[0], &hpts[1]);

  // Minute hand (ink, medium).
  hd.color = t->ink; hd.width = 4;
  lv_point_t me; hand_end(cx, cy, ang_min, HAND_M_LEN, &me);
  lv_point_t mpts[2] = { center, me };
  lv_draw_line(ctx, &hd, &mpts[0], &mpts[1]);

  // Second hand (accent, thin).
  hd.color = t->accent; hd.width = 2;
  lv_point_t se; hand_end(cx, cy, ang_sec, HAND_S_LEN, &se);
  lv_point_t spts[2] = { center, se };
  lv_draw_line(ctx, &hd, &spts[0], &spts[1]);

  // Center hub dot (accent filled circle).
  lv_draw_rect_dsc_t cd; lv_draw_rect_dsc_init(&cd);
  cd.bg_opa = LV_OPA_COVER; cd.bg_color = t->accent; cd.radius = LV_RADIUS_CIRCLE;
  lv_area_t hub = { (lv_coord_t)(cx - 6), (lv_coord_t)(cy - 6),
                    (lv_coord_t)(cx + 6), (lv_coord_t)(cy + 6) };
  lv_draw_rect(ctx, &cd, &hub);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  // Header: lowercase "beacon" eyebrow + date placeholder (matches lane top row).
  lv_obj_t* eyebrow = lv_label_create(page);
  lv_label_set_text(eyebrow, "beacon");
  lv_obj_set_style_text_font(eyebrow, t->f_mono, 0);
  lv_obj_set_style_text_color(eyebrow, t->ink_dim, 0);
  lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "wed 05 jun");
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  // Clock face: transparent custom-draw object centered, nudged up to leave room for readouts.
  s_dial = lv_obj_create(page);
  lv_obj_remove_style_all(s_dial);
  lv_obj_set_size(s_dial, DIAL_SIZE, DIAL_SIZE);
  lv_obj_align(s_dial, LV_ALIGN_CENTER, 0, -24);
  lv_obj_clear_flag(s_dial, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_dial, draw_face, LV_EVENT_DRAW_MAIN, NULL);

  // Small "12" marker at the top of the dial.
  lv_obj_t* twelve = lv_label_create(page);
  lv_label_set_text(twelve, "12");
  lv_obj_set_style_text_font(twelve, t->f_mono, 0);
  lv_obj_set_style_text_color(twelve, t->ink_dim, 0);
  lv_obj_align_to(twelve, s_dial, LV_ALIGN_TOP_MID, 0, 6);

  // Digital readouts row: temp + humidity, below the dial.
  s_temp = lv_label_create(page);
  lv_obj_set_style_text_font(s_temp, t->f_display, 0);
  lv_obj_set_style_text_color(s_temp, t->ink, 0);
  lv_obj_align(s_temp, LV_ALIGN_BOTTOM_MID, -64, -64);
  lv_label_set_text(s_temp, "--");

  lv_obj_t* temp_c = lv_label_create(page);
  lv_label_set_text(temp_c, "temp");
  lv_obj_set_style_text_font(temp_c, t->f_mono, 0);
  lv_obj_set_style_text_color(temp_c, t->ink_dim, 0);
  lv_obj_align(temp_c, LV_ALIGN_BOTTOM_MID, -64, -42);

  s_hum = lv_label_create(page);
  lv_obj_set_style_text_font(s_hum, t->f_display, 0);
  lv_obj_set_style_text_color(s_hum, t->ink, 0);
  lv_obj_align(s_hum, LV_ALIGN_BOTTOM_MID, 64, -64);
  lv_label_set_text(s_hum, "--");

  lv_obj_t* hum_c = lv_label_create(page);
  lv_label_set_text(hum_c, "humidity");
  lv_obj_set_style_text_font(hum_c, t->f_mono, 0);
  lv_obj_set_style_text_color(hum_c, t->ink_dim, 0);
  lv_obj_align(hum_c, LV_ALIGN_BOTTOM_MID, 64, -42);
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  lv_obj_invalidate(s_dial);  // repaint drawn hands from the live time
  weather_rec_t w = ds_get_weather();
  uint32_t now = now_s();

  char chip[24];
  if (sv_status(chip, sizeof(chip), &w.hdr, now)) {
    lv_label_set_text(s_status, chip);
    lv_obj_set_style_text_color(s_status, sv_severe(w.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    render_date(s_status);
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  }

  lv_color_t vcol = sv_dim(w.hdr.state) ? t->ink_dim : t->ink;

  if (sv_placeholder(w.hdr.state)) {
    lv_label_set_text(s_temp, "--");
    lv_label_set_text(s_hum, "--");
  } else {
    char tb[16]; snprintf(tb, sizeof(tb), "%.1f\xC2\xB0", w.temp_c);
    lv_label_set_text(s_temp, tb);
    char hb[16]; snprintf(hb, sizeof(hb), "%.0f%%", w.humidity_pct);
    lv_label_set_text(s_hum, hb);
  }
  lv_obj_set_style_text_color(s_temp, vcol, 0);
  lv_obj_set_style_text_color(s_hum, vcol, 0);
}

extern const screen_view_t home_analog_view = { build, update };
