#pragma once
#include <lvgl.h>

// A per-theme view of a screen: build() lays it out into the page, update() refreshes it from
// the DataStore. Each (screen x theme) pair provides one of these; the screen module dispatches
// to the active theme's view. Theme switch => the carousel rebuilds the page with the new view.
typedef struct {
  void (*build)(lv_obj_t* page);   // lay out this theme's design into the page
  void (*update)(void);            // refresh from the current DataStore snapshot (idempotent)
} screen_view_t;

// A screen in the carousel: dispatches build/update to the active theme's view.
typedef struct {
  const char* id;                       // "HOME","MARKETS","LIMITS","CLAUDE","NOW","SETTINGS"
  lv_obj_t*  (*build)(lv_obj_t* page);  // build the active theme's view into the page; returns page
  void       (*update)(void);           // update the active theme's view
} screen_module_t;
