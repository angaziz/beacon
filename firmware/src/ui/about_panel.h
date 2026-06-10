#pragma once
#include <stdbool.h>

// Read-only About modal (firmware version, build date, WiFi/BLE MAC, uptime, free heap),
// drawn on lv_layer_top above the carousel and themed from the active tokens. Opened by
// tapping any theme's Settings "About" row. Dismissed by Back, a tap anywhere on the panel,
// or the shake gesture (via ui_dismiss_top_overlay). Values are snapshotted at open.
#ifdef __cplusplus
extern "C" {
#endif
void about_panel_open(void);
bool about_panel_is_open(void);
void about_panel_close(void);
#ifdef __cplusplus
}
#endif
