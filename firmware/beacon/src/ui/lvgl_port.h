#pragma once
// LVGL 8.4 port: two partial draw buffers, flush -> display, indev <- touch.
// Buffer region (internal SRAM vs PSRAM) is chosen at boot per the heap floor.
bool lvgl_port_begin();   // call after display_begin() + touch_begin()
void lvgl_port_tick();    // pump from loop() on Core 1
