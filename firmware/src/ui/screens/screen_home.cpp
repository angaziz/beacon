#include "ui/screens/screen_home.h"
#include "ui/theme.h"
#include "core/fetch_task.h"

// Per-theme views (catalog order: editorial,hud,calm,blueprint,led,oscilloscope,analog).
extern const screen_view_t home_editorial_view, home_hud_view, home_calm_view,
  home_blueprint_view, home_led_view, home_oscilloscope_view, home_analog_view;
static const screen_view_t* V[] = {
  &home_editorial_view, &home_hud_view, &home_calm_view, &home_blueprint_view,
  &home_led_view, &home_oscilloscope_view, &home_analog_view,
};

static lv_obj_t* build(lv_obj_t* page) { V[theme_index()]->build(page); return page; }
static void update(void) { V[theme_index()]->update(); }
static void context(void) { fetch_task_refresh_now(); }   // long-press = re-fetch weather now (FR-PLAT-5)
const screen_module_t home_module = {"HOME", build, update, context};
