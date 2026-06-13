#pragma once
#include <lvgl.h>

// Screenshot harness (env:capture only): stitches the real RGB565 strips LVGL flushes to the panel
// into a full 466x466 frame and streams it over USB-CDC, so the host gets the exact pixels sent to
// the glass (no camera, no glare). Whole module compiles out of env:beacon (gated on BEACON_CAPTURE).
#if BEACON_CAPTURE

// Called from the LVGL flush_cb. While a sweep is armed, mirrors this strip into the full-frame
// buffer; harmless no-op otherwise (the strip still reaches the panel either way).
void capture_blit(const lv_area_t* a, const lv_color_t* px);

// Poll once per loop(): on receiving 'C' over Serial, sweeps every theme x screen and streams each
// frame to the host. Blocking for the duration of the sweep (UI is frozen on purpose).
void capture_service(void);

#endif
