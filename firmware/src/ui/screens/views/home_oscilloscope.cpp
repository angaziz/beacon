#include "ui/screen.h"
#include "ui/screens/screen_common.h"
#include "ui/styles.h"
#include "ui/state_view.h"
#include "ui/theme.h"
#include "ui/batt_chip.h"
#include "ui/screens/views/view_common.h"
#include "config/layout.h"
#include "core/datastore.h"
#include <Arduino.h>
#include <math.h>

// Oscilloscope / Signal HOME. Chrome (graticule + center axis) is drawn by the carousel.
// Scope header (CH1 . 50ms/DIV | BAT chip), big phosphor clock, a glowing sine WAVEFORM
// trace drawn via custom DRAW_MAIN lv_draw_line, and corner readouts TEMP / HUMID / SKY
// (each a dim eyebrow over a bright phosphor value).


static lv_obj_t *s_clock, *s_date, *s_trig;
static lv_obj_t *s_wave;
static lv_obj_t *s_temp, *s_humid, *s_sky;

static void update(void);

static void wave_cb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  const beacon_theme_t* t = theme_active(); if (!t) return;
  lv_area_t a; lv_obj_get_coords(o, &a);
  lv_coord_t w = lv_area_get_width(&a);
  lv_coord_t h = lv_area_get_height(&a);
  if (w < 4 || h < 4) return;
  lv_coord_t mid = a.y1 + h / 2;
  lv_coord_t amp = (h / 2) - 2;

  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
  ld.color = t->accent; ld.width = 3; ld.opa = LV_OPA_COVER;
  ld.round_start = 1; ld.round_end = 1;

  // ~2.5 cycles of a sine across the full width, sampled as a polyline.
  const int N = 48;
  lv_point_t prev;
  for (int i = 0; i <= N; i++) {
    float fx = (float)i / (float)N;
    lv_coord_t x = a.x1 + (lv_coord_t)(fx * (float)(w - 1));
    float ph = fx * 2.5f * 2.0f * (float)M_PI;
    lv_coord_t y = mid - (lv_coord_t)(sinf(ph) * (float)amp);
    lv_point_t cur = { x, y };
    if (i > 0) lv_draw_line(ctx, &ld, &prev, &cur);
    prev = cur;
  }
}

// One corner readout: dim eyebrow over a bright phosphor value. Returns the value label.
static lv_obj_t* make_readout(lv_obj_t* page, const beacon_theme_t* t, const char* eyebrow,
                              lv_align_t align, lv_text_align_t talign) {
  lv_obj_t* col = lv_obj_create(page);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(col, align,
               align == LV_ALIGN_BOTTOM_LEFT ? SAFE_INSET :
               (align == LV_ALIGN_BOTTOM_RIGHT ? -SAFE_INSET : 0),
               -SAFE_INSET);

  lv_obj_t* eb = lv_label_create(col);
  lv_label_set_text(eb, eyebrow);
  lv_obj_set_style_text_font(eb, t->f_mono, 0);
  lv_obj_set_style_text_color(eb, t->ink_dim, 0);
  lv_obj_set_style_text_align(eb, talign, 0);

  lv_obj_t* v = lv_label_create(col);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_font(v, t->f_display, 0);
  lv_obj_set_style_text_color(v, t->ink, 0);
  lv_obj_set_style_text_align(v, talign, 0);
  return v;
}

static void build(lv_obj_t* page) {
  const beacon_theme_t* t = theme_active();

  lv_obj_t* ch1 = lv_label_create(page);
  lv_label_set_text(ch1, "CH1 . 50ms/DIV");
  lv_obj_set_style_text_color(ch1, t->ink_dim, 0);
  lv_obj_set_style_text_font(ch1, t->f_mono, 0);
  lv_obj_align(ch1, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  s_trig = lv_label_create(page);
  lv_label_set_text(s_trig, "BAT . --");
  lv_obj_set_style_text_color(s_trig, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_trig, t->f_mono, 0);
  lv_obj_align(s_trig, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_clock = lv_label_create(page);
  lv_label_set_text(s_clock, "--:--");
  lv_obj_set_style_text_color(s_clock, t->ink, 0);
  lv_obj_set_style_text_font(s_clock, t->f_hero, 0);
  lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 36);

  s_date = lv_label_create(page);
  lv_label_set_text(s_date, "--");
  lv_obj_set_style_text_color(s_date, t->ink_dim, 0);
  lv_obj_set_style_text_font(s_date, t->f_mono, 0);
  lv_obj_align_to(s_date, s_clock, LV_ALIGN_OUT_BOTTOM_MID, 0, SPACE_S);

  s_wave = lv_obj_create(page);
  lv_obj_remove_style_all(s_wave);
  lv_obj_set_size(s_wave, SCREEN_W - 2 * SAFE_INSET, 96);
  lv_obj_align(s_wave, LV_ALIGN_CENTER, 0, 28);
  lv_obj_clear_flag(s_wave, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_wave, wave_cb, LV_EVENT_DRAW_MAIN, NULL);

  s_temp  = make_readout(page, t, "TEMP",  LV_ALIGN_BOTTOM_LEFT,  LV_TEXT_ALIGN_LEFT);
  s_humid = make_readout(page, t, "HUMID", LV_ALIGN_BOTTOM_MID,   LV_TEXT_ALIGN_CENTER);
  s_sky   = make_readout(page, t, "SKY",   LV_ALIGN_BOTTOM_RIGHT, LV_TEXT_ALIGN_RIGHT);

  update();
}

static void update(void) {
  const beacon_theme_t* t = theme_active(); if (!t) return;
  render_clock_ex(s_clock, s_date, "%a %d %b", txt_set);
  weather_rec_t w = ds_get_weather();
  uint32_t now = now_s();

  status_chip_update(s_trig, &w.hdr, now, t, true, "BAT . ", txt_set);

  bool ph = sv_placeholder(w.hdr.state);
  lv_color_t vc = sv_dim(w.hdr.state) ? t->ink_dim : t->ink;

  char buf[16];
  if (ph) {
    txt_set(s_temp, "--");
    txt_set(s_humid, "--");
  } else {
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", w.temp_c);
    txt_set(s_temp, buf);
    snprintf(buf, sizeof(buf), "%.0f%%", w.humidity_pct);
    txt_set(s_humid, buf);
  }
  // Condition has no decoded label source in this chunk => placeholder.
  txt_set(s_sky, "--");

  txt_color(s_temp, vc);
  txt_color(s_humid, vc);
  txt_color(s_sky, t->ink_dim);
}

extern const screen_view_t home_oscilloscope_view = { build, update };
