#include "ui/chrome.h"
#include "ui/theme.h"
#include "config/layout.h"

typedef enum { CH_NONE, CH_GRID, CH_BLUEPRINT, CH_DOTS, CH_GRATICULE } chrome_kind_t;

// theme index -> chrome kind (catalog order: editorial,hud,calm,blueprint,led,oscilloscope,analog)
static chrome_kind_t kind_for(uint8_t idx) {
  switch (idx) {
    case 1: return CH_GRID;        // Aerospace HUD
    case 3: return CH_BLUEPRINT;   // Blueprint
    case 4: return CH_DOTS;        // LED matrix
    case 5: return CH_GRATICULE;   // Oscilloscope
    default: return CH_NONE;       // editorial / calm / analog
  }
}

static void line(lv_draw_ctx_t* ctx, lv_draw_line_dsc_t* d, int x1, int y1, int x2, int y2) {
  lv_point_t a = {(lv_coord_t)x1, (lv_coord_t)y1}, b = {(lv_coord_t)x2, (lv_coord_t)y2};
  lv_draw_line(ctx, d, &a, &b);
}

static void grid(lv_draw_ctx_t* ctx, const lv_area_t* a, lv_color_t c, int sp, lv_opa_t opa) {
  lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.color = c; d.width = 1; d.opa = opa;
  for (int x = a->x1; x <= a->x2; x += sp) line(ctx, &d, x, a->y1, x, a->y2);
  for (int y = a->y1; y <= a->y2; y += sp) line(ctx, &d, a->x1, y, a->x2, y);
}

static void draw_cb(lv_event_t* e) {
  lv_obj_t* bg = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  const beacon_theme_t* t = theme_active(); if (!t) return;
  lv_area_t a; lv_obj_get_coords(bg, &a);
  int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;

  switch (kind_for(theme_index())) {
    case CH_GRID:
      grid(ctx, &a, t->line, 48, LV_OPA_COVER);
      break;

    case CH_BLUEPRINT: {
      grid(ctx, &a, t->line, 24, LV_OPA_50);
      lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.color = t->accent; d.width = 1; d.opa = LV_OPA_70;
      int L = 16, in = SAFE_INSET - 12;
      line(ctx, &d, a.x1+in, a.y1+in, a.x1+in+L, a.y1+in); line(ctx, &d, a.x1+in, a.y1+in, a.x1+in, a.y1+in+L); // TL
      line(ctx, &d, a.x2-in, a.y1+in, a.x2-in-L, a.y1+in); line(ctx, &d, a.x2-in, a.y1+in, a.x2-in, a.y1+in+L); // TR
      line(ctx, &d, a.x1+in, a.y2-in, a.x1+in+L, a.y2-in); line(ctx, &d, a.x1+in, a.y2-in, a.x1+in, a.y2-in-L); // BL
      line(ctx, &d, a.x2-in, a.y2-in, a.x2-in-L, a.y2-in); line(ctx, &d, a.x2-in, a.y2-in, a.x2-in, a.y2-in-L); // BR
      int ch = 130;
      line(ctx, &d, cx-ch, cy, cx+ch, cy); line(ctx, &d, cx, cy-ch, cx, cy+ch);   // crosshair
      break;
    }

    case CH_DOTS: {
      lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color = t->accent; d.bg_opa = LV_OPA_30; d.radius = LV_RADIUS_CIRCLE;
      for (int y = a.y1; y <= a.y2; y += 13)
        for (int x = a.x1; x <= a.x2; x += 13) {
          lv_area_t dot = {(lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x+1), (lv_coord_t)(y+1)};
          lv_draw_rect(ctx, &d, &dot);
        }
      break;
    }

    case CH_GRATICULE: {
      grid(ctx, &a, t->line, 42, LV_OPA_50);
      lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.color = t->accent; d.width = 1; d.opa = LV_OPA_60;
      line(ctx, &d, a.x1, cy, a.x2, cy);   // bright center axis
      break;
    }

    default: break;
  }
}

lv_obj_t* chrome_attach(lv_obj_t* page) {
  lv_obj_t* bg = lv_obj_create(page);
  lv_obj_remove_style_all(bg);
  lv_obj_set_size(bg, SCREEN_W, SCREEN_H);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(bg, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
  return bg;
}
