#include "ui/overlays.h"
#include "ui/theme_panel.h"
#include "ui/wifi_panel.h"
#include "ui/duration_panel.h"

bool ui_dismiss_top_overlay(void) {
  if (duration_panel_is_open()) { duration_panel_close(); return true; }
  if (theme_panel_is_open())    { theme_panel_close();    return true; }
  if (wifi_panel_is_open())     { wifi_panel_close();     return true; }
  return false;
}
