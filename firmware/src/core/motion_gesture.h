#pragma once
#include <stdint.h>
#include <stdbool.h>
// Pure motion-gesture detector (no Arduino/LVGL, host-tested). Fed one accel-magnitude sample at a
// time (milli-g) with a millisecond timestamp; emits at most one event per sample. It works on the
// JERK (|Δ magnitude|) so the 1 g rest baseline cancels and orientation does not matter.
//
// A "peak" is a jerk past `peak_mg`, debounced by `peak_gap_ms` so a single physical flick counts
// once. The FIRST peak of a burst is MOTION_WAKE (any deliberate motion -> wake the panel). Once
// `shake_peaks` peaks land inside `shake_window_ms`, MOTION_SHAKE fires and a `refractory_ms` quiet
// period follows (so one long shake is one event). The input controller routes by display state:
// asleep/dim + any event -> wake; awake + shake -> dismiss the open overlay.
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MOTION_NONE = 0, MOTION_WAKE = 1, MOTION_SHAKE = 2 } motion_event_t;

// Device defaults (tune on hardware). Poll cadence is ~60 ms (see ui/input.cpp).
#define MOTION_PEAK_MG          300   // jerk (milli-g) that counts as a motion peak
#define MOTION_PEAK_GAP_MS       80   // min spacing between counted peaks (debounce one jerk)
#define MOTION_SHAKE_WINDOW_MS 1000   // peaks within this window...
#define MOTION_SHAKE_PEAKS        4   // ...this many => shake (must be <= MOTION_PEAK_CAP)
#define MOTION_REFRACTORY_MS    700   // quiet period after a shake

#define MOTION_PEAK_CAP 8             // capacity of the recent-peak timestamp buffer

typedef struct {
  int16_t  peak_mg;
  uint16_t peak_gap_ms;
  uint16_t shake_window_ms;
  uint8_t  shake_peaks;
  uint16_t refractory_ms;

  int16_t  prev_mg;
  bool     have_prev;
  uint32_t peaks[MOTION_PEAK_CAP];   // timestamps of recent peaks, oldest-first
  uint8_t  peak_n;
  uint32_t last_peak_ms;
  uint32_t refractory_until;
  bool     refractory_active;
} motion_det_t;

void motion_config(motion_det_t* d, int16_t peak_mg, uint16_t peak_gap_ms,
                   uint16_t shake_window_ms, uint8_t shake_peaks, uint16_t refractory_ms);
void motion_init_default(motion_det_t* d);   // MOTION_* defaults above
void motion_reset(motion_det_t* d);          // clears runtime state, keeps config
motion_event_t motion_feed(motion_det_t* d, int16_t mag_mg, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
