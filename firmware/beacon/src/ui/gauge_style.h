#pragma once
// LVGL-free so both the host-testable theme catalog and the device theme/gauge code share it.
typedef enum {
  GAUGE_BAR, GAUGE_RING, GAUGE_CELL, GAUGE_WAVEFORM, GAUGE_MEASURE, GAUGE_BIGFIG, GAUGE_SUBDIAL
} gauge_style_t;
