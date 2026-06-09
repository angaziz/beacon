#pragma once
#include <lvgl.h>
#include <stdint.h>

// Single epoch clock for the UI (FR-PLAT-8 unification). Returns timekeep_now() so screen staleness
// ages use the SAME epoch the Core-0 fetchers stamp into hdr.last_updated. Defined in timekeep.cpp.
// Every view reads time through this; there are no per-view millis() clocks (would split-brain ages).
uint32_t now_s(void);

// Monotonic uptime in seconds (millis()/1000). Unlike now_s()/timekeep_now() (wall clock, which
// JUMPS on NTP/RTC sync), this never moves backward => safe for prompt lifecycle timeouts. Defined
// in timekeep.cpp.
uint32_t uptime_s(void);

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
