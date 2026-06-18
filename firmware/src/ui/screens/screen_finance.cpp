#include "ui/screens/screen_finance.h"
#include "ui/theme.h"
#include "ui/chrome.h"
#include "config/ticker_table.h"

extern const screen_view_t finance_editorial_view, finance_hud_view, finance_calm_view,
  finance_blueprint_view, finance_led_view, finance_oscilloscope_view, finance_analog_view;
static const screen_view_t* V[] = {
  &finance_editorial_view, &finance_hud_view, &finance_calm_view, &finance_blueprint_view,
  &finance_led_view, &finance_oscilloscope_view, &finance_analog_view,
};

// Track the ticker-table gen the current view was built against. A hub config swap bumps the gen
// (Core-0); the next Core-1 update tick rebuilds the view so its row count + names match the new set.
static lv_obj_t* s_page = nullptr;
static uint32_t  s_built_gen = 0;

static lv_obj_t* build(lv_obj_t* page) {
  s_page = page;
  s_built_gen = ticker_table_gen();
  V[theme_index()]->build(page);
  return page;
}

static void update(void) {
  uint32_t gen = ticker_table_gen();
  if (s_page && gen != s_built_gen) {
    // Rebuild against the new ticker set (Core-1 safe: only the existing LVGL lifecycle + accessors).
    // Views build row objects once and cache row pointers, so a count change needs a teardown, not a restyle.
    lv_obj_clean(s_page);
    chrome_attach(s_page);
    s_built_gen = gen;
    V[theme_index()]->build(s_page);
  }
  V[theme_index()]->update();
}
const screen_module_t finance_module = {"MARKETS", build, update};
