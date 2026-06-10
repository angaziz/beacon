#pragma once
#include <stdbool.h>
// Quick-brightness overlay (FR-PLAT-5 swipe-down). A centred card with a slider bound to the backlight
// + NVS, on a dimmed backdrop. Opened by a swipe-down gesture; closed by tap-out, shake (FR-PLAT-6),
// or auto-timeout. Modelled on pair_overlay (single shared instance on lv_layer_top).
#ifdef __cplusplus
extern "C" {
#endif
void brightness_overlay_open(void);
void brightness_overlay_close(void);
bool brightness_overlay_is_open(void);
void brightness_overlay_service(void);   // auto-close after a short idle; call from the LVGL loop
#ifdef __cplusplus
}
#endif
