#pragma once
#include <lvgl.h>

// Builds the swipe carousel from the six screen modules + the dot indicator, applies the initial
// theme, and starts the ~500 ms visible-screen update timer. Call after styles_init() + datastore_init().
void carousel_init(void);
int  carousel_current(void);     // current page index
lv_obj_t* carousel_root(void);   // the pager object (dev_seed attaches the long-press fault-injector here)
void carousel_set_swipe_enabled(bool en);   // suspend/restore horizontal swipe (e.g. while an overlay is open)
void carousel_invoke_context_action(void);  // run the current screen's long-press action, if any (FR-PLAT-5)
