#pragma once
#include <lvgl.h>

// Builds the swipe carousel from the five screen modules + the dot indicator, applies the initial
// theme, and starts the ~500 ms visible-screen update timer. Call after styles_init() + datastore_init().
void carousel_init(void);
int  carousel_current(void);     // current page index
lv_obj_t* carousel_root(void);   // the pager object (dev_seed attaches the long-press fault-injector here)
void carousel_set_swipe_enabled(bool en);   // suspend/restore horizontal swipe (e.g. while an overlay is open)
void carousel_set_tick_paused(bool paused);  // pause/resume the 500ms update tick (idle sleep, #60)
#if BEACON_CAPTURE
int  carousel_count(void);                   // number of screens (screenshot sweep)
const char* carousel_screen_id(int idx);     // canonical id of screen idx (screenshot filenames)
void carousel_goto(int idx);                 // make screen idx the visible/active page (no animation)
#endif
