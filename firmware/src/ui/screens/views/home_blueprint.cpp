// Blueprint / Schematic - HOME. Draftsman engineering drawing: technical DWG header,
// centered big clock figure, dimension callouts for temp + humidity. The grid, corner
// registration marks and crosshair reticle are drawn by the carousel chrome; do NOT redraw.
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

static lv_obj_t *s_clock, *s_date, *s_temp, *s_hum, *s_status;

// Clock + date from the time service. RTC time is always "live" (FR-HOME-3); show "--" until known.
// Date keeps the schematic dimension-caption look: hairline dashes flank the mono date.
static void render_clock(lv_obj_t* clock, lv_obj_t* date) {
  if (!timekeep_has_time()) { lv_label_set_text(clock, "--:--"); lv_label_set_text(date, "--"); return; }
  struct tm lt; timekeep_localtime(&lt);
  char hm[8];  strftime(hm, sizeof(hm), "%H:%M", &lt);            lv_label_set_text(clock, hm);
  char dt[24]; strftime(dt, sizeof(dt), "--- %a %d %b ---", &lt);
  for (char* p = dt; *p; ++p) *p = (char)toupper((unsigned char)*p);
  lv_label_set_text(date, dt);
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  // Technical header: DWG. BEACON-001 / HOME  ....  BAT chip (or state chip when non-live)
  lv_obj_t* dwg = lv_label_create(page);
  lv_label_set_text(dwg, "DWG. BEACON-001 / HOME");
  lv_obj_set_style_text_color(dwg, t->ink_dim, 0);
  lv_obj_set_style_text_font(dwg, t->f_mono, 0);
  lv_obj_align(dwg, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_label_set_text(s_status, "BAT --");
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_status, t->f_mono, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  // Centered big clock figure (placeholder; no real time source yet).
  s_clock = lv_label_create(page);
  lv_label_set_text(s_clock, "--:--");
  lv_obj_set_style_text_color(s_clock, t->ink, 0);
  lv_obj_set_style_text_font(s_clock, t->f_hero, 0);
  lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -10);

  // Dimension caption under the figure: hairline dashes flank a mono date string.
  s_date = lv_label_create(page);
  lv_label_set_text(s_date, "--- --  -- --- ---- ---");
  lv_obj_set_style_text_color(s_date, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_date, t->f_mono, 0);
  lv_obj_align_to(s_date, s_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, SPACE_L);

  // Dimension callouts row: + TEMP <v>C  (left)  RH + <v>% (right).
  s_temp = lv_label_create(page);
  lv_label_set_text(s_temp, "+ TEMP  --.- C");
  lv_obj_set_style_text_color(s_temp, t->accent, 0);
  lv_obj_set_style_text_font(s_temp, t->f_mono, 0);
  lv_obj_align(s_temp, LV_ALIGN_BOTTOM_LEFT, SAFE_INSET, -SAFE_INSET);

  s_hum = lv_label_create(page);
  lv_label_set_text(s_hum, "RH +  --%");
  lv_obj_set_style_text_color(s_hum, t->accent, 0);
  lv_obj_set_style_text_font(s_hum, t->f_mono, 0);
  lv_obj_align(s_hum, LV_ALIGN_BOTTOM_RIGHT, -SAFE_INSET, -SAFE_INSET);
}

static void update(void) {
  render_clock(s_clock, s_date);
  const beacon_theme_t* t = theme_active();
  weather_rec_t w = ds_get_weather();
  uint32_t now = now_s();

  char buf[32];
  if (sv_status(buf, sizeof(buf), &w.hdr, now)) {
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  } else {
    char bv[12]; lv_color_t bc = batt_chip(bv, sizeof(bv), true, t);
    snprintf(buf, sizeof(buf), "BAT %s", bv);
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, bc, 0);
  }

  bool dim = sv_dim(w.hdr.state);
  bool ph  = sv_placeholder(w.hdr.state);

  char tb[24], hb[20];
  if (ph) {
    snprintf(tb, sizeof(tb), "+ TEMP  --.- C");
    snprintf(hb, sizeof(hb), "RH +  --%%");
  } else {
    snprintf(tb, sizeof(tb), "+ TEMP  %.1f C", w.temp_c);
    snprintf(hb, sizeof(hb), "RH +  %.0f%%", w.humidity_pct);
  }
  lv_label_set_text(s_temp, tb);
  lv_label_set_text(s_hum, hb);

  lv_color_t c = dim ? t->ink_dim : t->accent;
  lv_obj_set_style_text_color(s_temp, c, 0);
  lv_obj_set_style_text_color(s_hum, c, 0);
}

extern const screen_view_t home_blueprint_view = { build, update };
