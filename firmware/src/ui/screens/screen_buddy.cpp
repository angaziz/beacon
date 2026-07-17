#include "ui/screens/screen_buddy.h"
#include "ui/screens/screen_module.h"
#include "ui/pal_panel.h"
#include "ui/theme.h"
#include <lvgl.h>

// Hand-expanded SCREEN_MODULE_SIMPLE(buddy, "CLAUDE", buddy_module) (see screen_module.h for what
// the macro generates): buddy needs one extra thing after the per-theme view builds -- the
// vertical-swipe gesture that opens the PAL mascot overlay (pal_panel.cpp) -- so it can't use the
// plain macro. Deviation is scoped to this file; the 7 per-theme views and the macro itself (still
// shared by home/finance/usage/settings) are untouched.

extern const screen_view_t buddy_editorial_view, buddy_hud_view, buddy_calm_view,
  buddy_blueprint_view, buddy_led_view, buddy_oscilloscope_view, buddy_analog_view;

namespace {

const screen_view_t* V[] = {
  &buddy_editorial_view, &buddy_hud_view, &buddy_calm_view,
  &buddy_blueprint_view, &buddy_led_view, &buddy_oscilloscope_view,
  &buddy_analog_view,
};

// A gesture only reaches `page` if nothing it started on stopped the bubble first: every plain
// lv_obj defaults to LV_OBJ_FLAG_GESTURE_BUBBLE when it has a parent (lv_obj.c), so a swipe
// starting on a session row/button/question-card bubbles up through its ancestors same as one
// starting on empty page background -- as long as `page` itself doesn't ALSO have the flag (it
// would just keep bubbling past it). Clearing GESTURE_BUBBLE on `page` below is what makes it the
// landing point either way, with no per-view/per-row changes needed.
void gesture_cb(lv_event_t*) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) return;
  // This press was a swipe, not a tap: stop it from also firing CLICKED on whatever row/button it
  // started on (e.g. auto-approving a permission prompt) once released.
  lv_indev_wait_release(lv_indev_get_act());
  pal_panel_open();
}

lv_obj_t* build(lv_obj_t* page) {
  V[theme_index()]->build(page);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(page, gesture_cb, LV_EVENT_GESTURE, NULL);
  return page;
}

void update(void) { V[theme_index()]->update(); }

}  // namespace

const screen_module_t buddy_module = {"CLAUDE", build, update};
