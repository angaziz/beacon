#include "ui/screens/screen_buddy.h"
#include "ui/theme.h"

extern const screen_view_t buddy_editorial_view, buddy_hud_view, buddy_calm_view,
  buddy_blueprint_view, buddy_led_view, buddy_oscilloscope_view, buddy_analog_view;
static const screen_view_t* V[] = {
  &buddy_editorial_view, &buddy_hud_view, &buddy_calm_view, &buddy_blueprint_view,
  &buddy_led_view, &buddy_oscilloscope_view, &buddy_analog_view,
};

static lv_obj_t* build(lv_obj_t* page) { V[theme_index()]->build(page); return page; }
static void update(void) { V[theme_index()]->update(); }
const screen_module_t buddy_module = {"CLAUDE", build, update};
