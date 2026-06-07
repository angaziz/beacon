#include "ui/screens/screen_nowplaying.h"
#include "ui/theme.h"

extern const screen_view_t nowplaying_editorial_view, nowplaying_hud_view, nowplaying_calm_view,
  nowplaying_blueprint_view, nowplaying_led_view, nowplaying_oscilloscope_view, nowplaying_analog_view;
static const screen_view_t* V[] = {
  &nowplaying_editorial_view, &nowplaying_hud_view, &nowplaying_calm_view, &nowplaying_blueprint_view,
  &nowplaying_led_view, &nowplaying_oscilloscope_view, &nowplaying_analog_view,
};

static lv_obj_t* build(lv_obj_t* page) { V[theme_index()]->build(page); return page; }
static void update(void) { V[theme_index()]->update(); }
const screen_module_t nowplaying_module = {"NOW", build, update};
