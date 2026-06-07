#include "ui/screens/screen_finance.h"
#include "ui/theme.h"

extern const screen_view_t finance_editorial_view, finance_hud_view, finance_calm_view,
  finance_blueprint_view, finance_led_view, finance_oscilloscope_view, finance_analog_view;
static const screen_view_t* V[] = {
  &finance_editorial_view, &finance_hud_view, &finance_calm_view, &finance_blueprint_view,
  &finance_led_view, &finance_oscilloscope_view, &finance_analog_view,
};

static lv_obj_t* build(lv_obj_t* page) { V[theme_index()]->build(page); return page; }
static void update(void) { V[theme_index()]->update(); }
const screen_module_t finance_module = {"MARKETS", build, update};
