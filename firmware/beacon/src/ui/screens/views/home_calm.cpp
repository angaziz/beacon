// Calm Futurism (Nothing-esque) HOME view.
// directions.html lane 04: top row "[dot] beacon / wed --", centered big Doto clock,
// dim dateline, two centered weather readouts (temp / humidity). Sparse, white-on-black,
// one faint red accent. Background + chrome are drawn by the carousel.
#include "ui/screen.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>
static void update(void);

static inline uint32_t now_s() { return (uint32_t)(millis() / 1000); }

static lv_obj_t *s_dot, *s_brand, *s_topright;
static lv_obj_t *s_clock, *s_date;
static lv_obj_t *s_temp, *s_hum;
static lv_obj_t *s_status;

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  // Top row: [dot] beacon  ...  wed -- (placeholder day)
  s_dot = lv_obj_create(page);
  lv_obj_remove_style_all(s_dot);
  lv_obj_set_size(s_dot, 8, 8);
  lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_dot, t->accent, 0);
  lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, SAFE_INSET + 4, SAFE_INSET + 12);

  s_brand = lv_label_create(page);
  lv_label_set_text(s_brand, "beacon");
  lv_obj_set_style_text_font(s_brand, t->f_body, 0);
  lv_obj_set_style_text_color(s_brand, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_brand, 3, 0);
  lv_obj_align(s_brand, LV_ALIGN_TOP_LEFT, SAFE_INSET + 20, SAFE_INSET + 8);

  s_topright = lv_label_create(page);
  lv_label_set_text(s_topright, "wed --");
  lv_obj_set_style_text_font(s_topright, t->f_body, 0);
  lv_obj_set_style_text_color(s_topright, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_topright, 3, 0);
  lv_obj_align(s_topright, LV_ALIGN_TOP_RIGHT, -(SAFE_INSET + 4), SAFE_INSET + 8);

  // Centered Doto clock (placeholder, no real time source).
  s_clock = lv_label_create(page);
  lv_label_set_text(s_clock, "--:--");
  lv_obj_set_style_text_font(s_clock, t->f_hero, 0);
  lv_obj_set_style_text_color(s_clock, t->ink, 0);
  lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -28);

  s_date = lv_label_create(page);
  lv_label_set_text(s_date, "-- -- ----");
  lv_obj_set_style_text_font(s_date, t->f_body, 0);
  lv_obj_set_style_text_color(s_date, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_date, 4, 0);
  lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 36);

  // Two centered weather readouts near the lower safe band.
  s_temp = lv_label_create(page);
  lv_obj_set_style_text_font(s_temp, t->f_display, 0);
  lv_obj_set_style_text_color(s_temp, t->ink, 0);
  lv_obj_align(s_temp, LV_ALIGN_BOTTOM_MID, -56, -(SAFE_INSET + 20));

  lv_obj_t* temp_k = lv_label_create(page);
  lv_label_set_text(temp_k, "temp");
  lv_obj_set_style_text_font(temp_k, t->f_body, 0);
  lv_obj_set_style_text_color(temp_k, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(temp_k, 2, 0);
  lv_obj_align(temp_k, LV_ALIGN_BOTTOM_MID, -56, -SAFE_INSET);

  s_hum = lv_label_create(page);
  lv_obj_set_style_text_font(s_hum, t->f_display, 0);
  lv_obj_set_style_text_color(s_hum, t->ink, 0);
  lv_obj_align(s_hum, LV_ALIGN_BOTTOM_MID, 56, -(SAFE_INSET + 20));

  lv_obj_t* hum_k = lv_label_create(page);
  lv_label_set_text(hum_k, "humidity");
  lv_obj_set_style_text_font(hum_k, t->f_body, 0);
  lv_obj_set_style_text_color(hum_k, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(hum_k, 2, 0);
  lv_obj_align(hum_k, LV_ALIGN_BOTTOM_MID, 56, -SAFE_INSET);

  // Status chip (non-live only), kept faint in the top-right slot area.
  s_status = lv_label_create(page);
  lv_obj_set_style_text_font(s_status, t->f_body, 0);
  lv_obj_set_style_text_color(s_status, t->ink_dim, 0);
  lv_obj_set_style_text_letter_space(s_status, 2, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -(SAFE_INSET + 4), SAFE_INSET + 26);
  lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active();
  weather_rec_t w = ds_get_weather();
  uint32_t now = now_s();

  char buf[24];
  bool ph = sv_placeholder(w.hdr.state);
  bool dim = sv_dim(w.hdr.state);

  if (ph) {
    lv_label_set_text(s_temp, "--");
    lv_label_set_text(s_hum, "--");
  } else {
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", w.temp_c);
    lv_label_set_text(s_temp, buf);
    snprintf(buf, sizeof(buf), "%.0f%%", w.humidity_pct);
    lv_label_set_text(s_hum, buf);
  }

  lv_color_t vc = dim ? t->ink_dim : t->ink;
  lv_obj_set_style_text_color(s_temp, vc, 0);
  lv_obj_set_style_text_color(s_hum, vc, 0);

  char sbuf[24];
  if (sv_status(sbuf, sizeof(sbuf), &w.hdr, now)) {
    lv_label_set_text(s_status, sbuf);
    lv_obj_set_style_text_color(s_status, sv_severe(w.hdr.state) ? t->down : t->ink_dim, 0);
    lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
  }
}

extern const screen_view_t home_calm_view = { build, update };
