#pragma once
#include <stdint.h>
class Arduino_GFX;
// CO5300 AMOLED over QSPI. display_begin() assumes power_begin() already ran.
bool display_begin();
void display_brightness(uint8_t level);                 // raw DCS 0x51 (no GFX API in 1.6.4)
void display_set_on(bool on);                           // DCS 0x29/0x28 (panel on/off) for idle sleep
void display_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* px);
Arduino_GFX* display_gfx();                              // for the cyan-border test (Task 1)
