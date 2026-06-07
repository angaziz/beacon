#pragma once
#include <lvgl.h>

typedef struct {
  const char* id;                       // eyebrow id: "HOME","MARKETS","LIMITS","CLAUDE","NOW","SETTINGS"
  lv_obj_t*  (*build)(lv_obj_t* page);  // build screen content into its page container; returns the page
  void       (*update)(void);           // re-render from current DataStore snapshot (idempotent, read-only)
} screen_module_t;
