#include "ui/screens/screen_usage.h"
#include "ui/theme.h"

extern const screen_view_t usage_editorial_view, usage_hud_view, usage_calm_view,
  usage_blueprint_view, usage_led_view, usage_oscilloscope_view, usage_analog_view;
static const screen_view_t* V[] = {
  &usage_editorial_view, &usage_hud_view, &usage_calm_view, &usage_blueprint_view,
  &usage_led_view, &usage_oscilloscope_view, &usage_analog_view,
};

static lv_obj_t* build(lv_obj_t* page) { V[theme_index()]->build(page); return page; }
static void update(void) { V[theme_index()]->update(); }
const screen_module_t usage_module = {"LIMITS", build, update};
