#pragma once
#include <stdio.h>
#include <ctype.h>
#include "ui/theme.h"
#include "hal/power.h"

// Battery value chip for the home top-right slot. Fills buf with "85%", "85%+" (charging),
// "USB" (on USB, no battery) or "--" (unknown), and returns the slot color: accent while
// charging, down when low (<=20%), else ink_dim. caps=false lowercases "USB" for the
// lowercase theme voices (analog, calm); digits/% are unaffected.
static inline lv_color_t batt_chip(char* buf, size_t n, bool caps, const beacon_theme_t* t) {
  int pct = power_battery_pct();
  bool chg = power_charging();
  if (pct >= 0) snprintf(buf, n, "%d%%%s", pct, chg ? "+" : "");
  else          snprintf(buf, n, "%s", chg ? "USB" : "--");
  if (!caps) for (char* p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p);
  return chg ? t->accent : (pct >= 0 && pct <= 20 ? t->down : t->ink_dim);
}
