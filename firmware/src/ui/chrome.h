#pragma once
#include <lvgl.h>
// Per-theme background chrome (grid / blueprint / dot-matrix / graticule), drawn behind a page's
// content via a custom LV_EVENT_DRAW_MAIN callback that reads the active theme. Attach as the FIRST
// child of a page so it paints under the content. Invalidated on theme switch (styles_apply).
lv_obj_t* chrome_attach(lv_obj_t* page);
