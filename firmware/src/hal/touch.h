#pragma once
#include <stdint.h>
// CST92xx capacitive touch over the shared I2C bus. touch_begin() assumes
// power_begin() already ran (Wire is up) and display_begin() already pulsed
// the shared reset line (see touch.cpp for why reset is left to the display).
bool touch_begin();                       // true if the controller answered on I2C
bool touch_read(int16_t* x, int16_t* y);  // true if a finger is down; coords in 0..SCREEN_W/H-1
