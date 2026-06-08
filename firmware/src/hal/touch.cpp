#include "touch.h"
#include <Wire.h>
#include "TouchDrvCSTXXX.hpp"   // SensorLib umbrella header that pulls in TouchDrvCST92xx
#include "config/pins.h"
#include "config/layout.h"

// SensorLib's mirror math is `out = MAX - raw`, so MAX must equal the sensor's
// native coordinate ceiling for the reflection to land correctly. The CST92xx
// on this panel reports in a 480-wide space (same value Waveshare's example
// uses), so we keep 480 here and scale down to the visible panel in touch_read.
static const uint16_t TOUCH_SENSOR_MAX = 480;

static TouchDrvCST92xx s_touch;

bool touch_begin() {
  // Reset is deliberately NOT handed to the driver: TP_RST shares GPIO with
  // LCD_RESET (both GPIO2). display_begin() already pulsed that line during
  // panel bring-up, which also reset the touch IC. Passing the real pin here
  // would make begin()'s internal reset() drive GPIO2 LOW and wipe the live
  // display, so we pass -1 and only wire up the INT pin.
  s_touch.setPins(-1, PIN_TOUCH_INT);

  // Wire is already begun by power_begin(); this overload re-applies the same
  // pins (idempotent on ESP32) and simply attaches the driver to the bus.
  if (!s_touch.begin(Wire, ADDR_TOUCH, PIN_IIC_SDA, PIN_IIC_SCL)) {
    return false;
  }

  // Orientation to match the panel mounting (same as Waveshare's example).
  s_touch.setMaxCoordinates(TOUCH_SENSOR_MAX, TOUCH_SENSOR_MAX);
  s_touch.setSwapXY(true);
  s_touch.setMirrorXY(true, false);
  return true;
}

bool touch_read(int16_t* x, int16_t* y) {
  int16_t raw_x[1];
  int16_t raw_y[1];

  // getPoint returns the number of fingers down (0 = released) and writes
  // swapped/mirrored coordinates in the 0..TOUCH_SENSOR_MAX-1 range.
  if (s_touch.getPoint(raw_x, raw_y, 1) == 0) {
    return false;
  }

  // Scale the sensor's 480-wide space onto the visible panel (SCREEN_W/H).
  *x = (int16_t)((int32_t)raw_x[0] * SCREEN_W / TOUCH_SENSOR_MAX);
  *y = (int16_t)((int32_t)raw_y[0] * SCREEN_H / TOUCH_SENSOR_MAX);
  return true;
}
