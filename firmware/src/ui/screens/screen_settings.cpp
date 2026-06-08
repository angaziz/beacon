#include "ui/screens/screen_settings.h"
#include "ui/theme.h"

extern const screen_view_t settings_editorial_view, settings_hud_view, settings_calm_view,
  settings_blueprint_view, settings_led_view, settings_oscilloscope_view, settings_analog_view;
static const screen_view_t* V[] = {
  &settings_editorial_view, &settings_hud_view, &settings_calm_view, &settings_blueprint_view,
  &settings_led_view, &settings_oscilloscope_view, &settings_analog_view,
};

static lv_obj_t* build(lv_obj_t* page) { V[theme_index()]->build(page); return page; }
static void update(void) { V[theme_index()]->update(); }
const screen_module_t settings_module = {"SETTINGS", build, update};
