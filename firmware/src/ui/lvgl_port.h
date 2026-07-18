#pragma once
#include <stdint.h>
// LVGL 8.4 port: one partial draw buffer, flush -> display, indev <- touch.
// Buffer region (internal SRAM vs PSRAM) is chosen at boot per the heap floor.
bool lvgl_port_begin();   // call after display_begin() + touch_begin()
void lvgl_port_tick();    // pump from loop() on Core 1

// Screen rotation (core/orientation.h quadrants: 0/90/180/270 as 0..3). The CO5300 cannot rotate in
// hardware -- its MADCTL only flips axes, no MV bit -- so this is LVGL's software rotation. Panel is
// square (466x466), so nothing reflows: every screen keeps rendering in the same logical 466x466
// space, and LVGL remaps both the framebuffer and the touch coordinates itself (see the note in
// indev_read_cb -- the port must NOT remap touch on top of that). Policy (auto vs fixed, and when a
// change is allowed to land) lives in ui/rotation.h; this is the mechanism only.
void    lvgl_port_set_rotation(uint8_t rot);
uint8_t lvgl_port_rotation(void);

// Whether a finger is currently on the glass. LVGL 8 has no public getter for indev press state, and
// ui/rotation.cpp needs it to hold a turn until the user lets go, so the read_cb records it here.
bool    lvgl_port_touch_down(void);
