#include "ui/styles.h"
#include "ui/theme.h"

app_styles_t S;

void styles_rebuild(const beacon_theme_t* t) {
  lv_style_set_bg_color(&S.screen, t->bg);
  lv_style_set_bg_opa(&S.screen, LV_OPA_COVER);

  lv_style_set_text_font(&S.eyebrow, t->f_mono);
  lv_style_set_text_color(&S.eyebrow, t->accent);

  lv_style_set_text_font(&S.slot, t->f_mono);
  lv_style_set_text_color(&S.slot, t->ink_dim);

  lv_style_set_text_font(&S.display, t->f_display);
  lv_style_set_text_color(&S.display, t->ink);

  lv_style_set_text_font(&S.hero, t->f_hero);
  lv_style_set_text_color(&S.hero, t->ink);

  lv_style_set_text_font(&S.body, t->f_body);
  lv_style_set_text_color(&S.body, t->ink);

  lv_style_set_text_color(&S.up, t->up);
  lv_style_set_text_color(&S.down, t->down);
  lv_style_set_text_color(&S.accent, t->accent);

  lv_style_set_bg_color(&S.hairline, t->line);
  lv_style_set_bg_opa(&S.hairline, LV_OPA_COVER);

  lv_style_set_text_color(&S.dim, t->ink_dim);

  lv_obj_report_style_change(NULL);   // re-evaluate every object using these styles
  lv_obj_invalidate(lv_scr_act());    // force chrome (custom-draw, no shared style) to repaint too
}

void styles_init(void) {
  lv_style_init(&S.screen);  lv_style_init(&S.eyebrow); lv_style_init(&S.slot);
  lv_style_init(&S.display); lv_style_init(&S.hero);    lv_style_init(&S.body);
  lv_style_init(&S.up);      lv_style_init(&S.down);    lv_style_init(&S.accent);
  lv_style_init(&S.hairline);lv_style_init(&S.dim);
}
