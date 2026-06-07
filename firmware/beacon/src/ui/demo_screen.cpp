#include "ui/demo_screen.h"
#include <lvgl.h>
#include "ui/theme.h"
#include "ui/theme_catalog.h"
#include "ui/gauge.h"
#include "config/layout.h"

// Rebuild hook: frozen teardown order — clear children, strip styles on the screen itself,
// then rebuild from the new tokens (one-theme-resident; heap must hold across switches).
static void build(const beacon_theme_t* th) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, th->bg, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t* eb = lv_label_create(scr);                 // eyebrow (mono)
  lv_label_set_text_fmt(eb, "BEACON / %s", th->id);
  lv_obj_set_style_text_color(eb, th->ink_dim, 0);
  lv_obj_set_style_text_font(eb, th->f_mono, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  lv_obj_t* nm = lv_label_create(scr);                 // theme name (display font)
  lv_label_set_text(nm, th->id);
  lv_obj_set_style_text_color(nm, th->ink, 0);
  lv_obj_set_style_text_font(nm, th->f_display, 0);
  lv_obj_align(nm, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 24);

  lv_obj_t* hero = lv_label_create(scr);               // hero figure (hero font, accent)
  lv_label_set_text(hero, "42%");
  lv_obj_set_style_text_color(hero, th->accent, 0);
  lv_obj_set_style_text_font(hero, th->f_hero, 0);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t* g = gauge_render(scr, th, 42);             // gauge (per theme style)
  lv_obj_align(g, LV_ALIGN_CENTER, 0, 70);

  lv_obj_t* fin = lv_label_create(scr);                // finance-style row (body, up color)
  lv_label_set_text(fin, "S&P 500   +1.2%");
  lv_obj_set_style_text_color(fin, th->up, 0);
  lv_obj_set_style_text_font(fin, th->f_body, 0);
  lv_obj_align(fin, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET - 20);

  lv_obj_t* hint = lv_label_create(scr);               // hint (mono)
  lv_label_set_text(hint, "tap to cycle theme");
  lv_obj_set_style_text_color(hint, th->ink_dim, 0);
  lv_obj_set_style_text_font(hint, th->f_mono, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);
}

static void tap_cb(lv_event_t* e) {
  (void)e;
  theme_set((uint8_t)((theme_index() + 1) % THEME_COUNT));
}

void demo_screen_init(void) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);     // persists across rebuilds (event, not style)
  lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);
  theme_on_apply(build);
  theme_set(0);   // Editorial; triggers build()
}
