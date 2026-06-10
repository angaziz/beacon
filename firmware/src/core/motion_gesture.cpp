#include "core/motion_gesture.h"

static inline int16_t absdiff(int16_t a, int16_t b) { int d = (int)a - (int)b; return (int16_t)(d < 0 ? -d : d); }

void motion_reset(motion_det_t* d) {
  d->prev_mg = 0;
  d->have_prev = false;
  d->peak_n = 0;
  d->last_peak_ms = 0;
  d->refractory_until = 0;
  d->refractory_active = false;
}

void motion_config(motion_det_t* d, int16_t peak_mg, uint16_t peak_gap_ms,
                   uint16_t shake_window_ms, uint8_t shake_peaks, uint16_t refractory_ms) {
  d->peak_mg = peak_mg;
  d->peak_gap_ms = peak_gap_ms;
  d->shake_window_ms = shake_window_ms;
  d->shake_peaks = shake_peaks > MOTION_PEAK_CAP ? MOTION_PEAK_CAP : shake_peaks;
  d->refractory_ms = refractory_ms;
  motion_reset(d);
}

void motion_init_default(motion_det_t* d) {
  motion_config(d, MOTION_PEAK_MG, MOTION_PEAK_GAP_MS, MOTION_SHAKE_WINDOW_MS,
                MOTION_SHAKE_PEAKS, MOTION_REFRACTORY_MS);
}

// Drop peak timestamps older than the shake window relative to `now`.
static void prune(motion_det_t* d, uint32_t now) {
  uint8_t keep = 0;
  for (uint8_t i = 0; i < d->peak_n; i++)
    if (now - d->peaks[i] < d->shake_window_ms) d->peaks[keep++] = d->peaks[i];
  d->peak_n = keep;
}

motion_event_t motion_feed(motion_det_t* d, int16_t mag_mg, uint32_t t_ms) {
  int16_t jerk = d->have_prev ? absdiff(mag_mg, d->prev_mg) : 0;
  d->prev_mg = mag_mg;
  d->have_prev = true;

  if (d->refractory_active) {
    if (t_ms >= d->refractory_until) d->refractory_active = false;  // monotonic millis; wrap (~49d) ignored
    else return MOTION_NONE;   // swallow everything (incl. baseline jerk) until the quiet period ends
  }

  if (jerk < d->peak_mg) return MOTION_NONE;
  if (d->last_peak_ms && (t_ms - d->last_peak_ms) < d->peak_gap_ms) return MOTION_NONE;  // debounce one jerk
  d->last_peak_ms = t_ms;

  prune(d, t_ms);
  if (d->peak_n < MOTION_PEAK_CAP) d->peaks[d->peak_n++] = t_ms;

  if (d->peak_n >= d->shake_peaks) {
    d->peak_n = 0;
    d->refractory_until = t_ms + d->refractory_ms;
    d->refractory_active = true;
    return MOTION_SHAKE;
  }
  return MOTION_WAKE;
}
