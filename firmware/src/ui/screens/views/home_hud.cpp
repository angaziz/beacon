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

// Aerospace HUD / Home. Topbar SYS . ONLINE | BAT chip, big centered clock with superscript
// seconds, date below, centered weather row (temp | humidity | condition) with hairline separators.
// Grid background is drawn by chrome; this view only adds content within SAFE_INSET.


static lv_obj_t *s_status;     // top-right slot (battery / state chip)
static lv_obj_t *s_clock, *s_date;
static lv_obj_t *s_temp, *s_temp_u;
static lv_obj_t *s_hum;
static lv_obj_t *s_cond;

// Clock + date from the time service. RTC time is always "live" (FR-HOME-3); show "--" until known.
static void render_clock(lv_obj_t* clock, lv_obj_t* date) {
  if (!timekeep_has_time()) { lv_label_set_text(clock, "--:--"); lv_label_set_text(date, "--"); return; }
  struct tm lt; timekeep_localtime(&lt);
  char hm[8];  strftime(hm, sizeof(hm), "%H:%M", &lt);            lv_label_set_text(clock, hm);
  char dt[24]; strftime(dt, sizeof(dt), "%a . %d %b", &lt);
  for (char* p = dt; *p; ++p) *p = (char)toupper((unsigned char)*p);
  lv_label_set_text(date, dt);
}

static lv_obj_t* row(lv_obj_t* parent) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return r;
}

static lv_obj_t* sep(lv_obj_t* parent, const beacon_theme_t* t) {
  lv_obj_t* s = lv_obj_create(parent);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, 1, 30);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s, t->line, 0);
  return s;
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  // Topbar: SYS . ONLINE (left, accent on ONLINE) + status slot (right).
  lv_obj_t* sys = lv_label_create(page);
  lv_obj_add_style(sys, &S.slot, 0);
  lv_obj_set_style_text_color(sys, t->accent, 0);
  lv_label_set_text(sys, "SYS . ONLINE");
  lv_obj_align(sys, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(page);
  lv_obj_add_style(s_status, &S.slot, 0);
  lv_label_set_text(s_status, "BAT . --");
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  // Center clock + date, driven live by the time service in update().
  s_clock = lv_label_create(page);
  lv_obj_set_style_text_font(s_clock, t->f_hero, 0);
  lv_obj_set_style_text_color(s_clock, t->ink, 0);
  lv_label_set_text(s_clock, "--:--");
  lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -30);

  s_date = lv_label_create(page);
  lv_obj_add_style(s_date, &S.slot, 0);
  lv_label_set_text(s_date, "-- . -- ---");
  lv_obj_align_to(s_date, s_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);

  // Weather row near bottom (inside safe area, above the dot indicator band).
  lv_obj_t* wx = row(page);
  lv_obj_set_size(wx, SCREEN_W - 2 * SAFE_INSET, 40);
  lv_obj_set_style_pad_column(wx, 24, 0);
  lv_obj_align(wx, LV_ALIGN_BOTTOM_MID, 0, -(SAFE_INSET + 24));

  lv_obj_t* tempbox = row(wx);
  lv_obj_set_size(tempbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  s_temp = lv_label_create(tempbox);
  lv_obj_set_style_text_font(s_temp, t->f_display, 0);
  lv_obj_set_style_text_color(s_temp, t->accent2, 0);   // amber temperature
  lv_label_set_text(s_temp, "--");
  s_temp_u = lv_label_create(tempbox);
  lv_obj_add_style(s_temp_u, &S.slot, 0);
  lv_label_set_text(s_temp_u, " \xC2\xB0""C");

  sep(wx, t);

  lv_obj_t* humbox = row(wx);
  lv_obj_set_size(humbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  s_hum = lv_label_create(humbox);
  lv_obj_set_style_text_font(s_hum, t->f_display, 0);
  lv_obj_set_style_text_color(s_hum, t->ink, 0);
  lv_label_set_text(s_hum, "--");
  lv_obj_t* hum_u = lv_label_create(humbox);
  lv_obj_add_style(hum_u, &S.slot, 0);
  lv_label_set_text(hum_u, " %RH");

  sep(wx, t);

  s_cond = lv_label_create(wx);
  lv_obj_add_style(s_cond, &S.slot, 0);
  lv_label_set_text(s_cond, "--");
}

static void update(void) {
  render_clock(s_clock, s_date);
  weather_rec_t w = ds_get_weather();
  uint32_t now = now_s();
  const beacon_theme_t* t = theme_active();

  char buf[24];
  if (sv_status(buf, sizeof(buf), &w.hdr, now)) {
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, sv_severe(w.hdr.state) ? t->down : t->ink_dim, 0);
  } else {
    char bv[12]; lv_color_t bc = batt_chip(bv, sizeof(bv), true, t);
    snprintf(buf, sizeof(buf), "BAT . %s", bv);
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, bc, 0);
  }

  bool ph = sv_placeholder(w.hdr.state);
  bool dim = sv_dim(w.hdr.state);

  if (ph) {
    lv_label_set_text(s_temp, "--");
    lv_label_set_text(s_hum, "--");
  } else {
    char tb[16];
    snprintf(tb, sizeof(tb), "%.1f", w.temp_c);
    lv_label_set_text(s_temp, tb);
    snprintf(tb, sizeof(tb), "%.0f", w.humidity_pct);
    lv_label_set_text(s_hum, tb);
  }
  lv_obj_set_style_text_color(s_temp, dim ? t->ink_dim : t->accent2, 0);
  lv_obj_set_style_text_color(s_hum, dim ? t->ink_dim : t->ink, 0);

  // Condition label has no string mapping in this chunk => keep placeholder.
  lv_label_set_text(s_cond, "--");
}

extern const screen_view_t home_hud_view = { build, update };
