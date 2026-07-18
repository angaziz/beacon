#include "core/orientation.h"
#include <math.h>

// Tunables (refine on hardware):
#define ORIENT_FLAT_G    0.35f   // in-plane |g| below this => lying flat, direction is noise; hold
#define ORIENT_HYST      1.5f    // winning axis must beat the other by this factor (~10 deg dead band)
#define ORIENT_DWELL_MS  700     // a new quadrant must hold this long before it is committed

static uint8_t  s_stable;
static uint8_t  s_cand;
static uint32_t s_cand_since;
static bool     s_have_cand;
static bool     s_changed;

void orient_reset(uint8_t initial) {
  s_stable = initial & 3;
  s_cand = s_stable; s_cand_since = 0; s_have_cand = false; s_changed = false;
}

void orient_feed(float ax, float ay, float az, uint32_t t_ms) {
  (void)az;   // only the in-plane components carry orientation; az just makes the vector flat-ish
  float mx = fabsf(ax), my = fabsf(ay);

  // Flat on the desk: the in-plane vector is too short for its direction to mean anything.
  if (mx * mx + my * my < ORIENT_FLAT_G * ORIENT_FLAT_G) { s_have_cand = false; return; }

  // ORIENT_NOTE: measured on hardware, not assumed -- this is the QMI8658's mounting on this panel.
  // Gravity direction in the accel frame maps to the compensating rotation as:
  //   +y (device upright, natural)   -> 0     -x (right side down) -> 90
  //   -y (device upside down)        -> 180   +x (left side down)  -> 270
  // Confirmed by two observations: resting on its left side committed 270 and rendered correctly,
  // while upright rendered 180 degrees off. Nothing else in the tree encodes accel axes.
  uint8_t cand;
  if (my > mx * ORIENT_HYST)      cand = (ay > 0.0f) ? ORIENT_0   : ORIENT_180;
  else if (mx > my * ORIENT_HYST) cand = (ax > 0.0f) ? ORIENT_270 : ORIENT_90;
  else { s_have_cand = false; return; }   // inside the diagonal dead band: no candidate at all

  if (cand == s_stable) { s_have_cand = false; return; }   // already there; drop any pending switch

  if (!s_have_cand || cand != s_cand) { s_cand = cand; s_cand_since = t_ms; s_have_cand = true; return; }
  if ((t_ms - s_cand_since) >= ORIENT_DWELL_MS) {
    s_stable = cand; s_changed = true; s_have_cand = false;
  }
}

uint8_t orient_stable(void) { return s_stable; }

bool orient_take_change(void) { bool c = s_changed; s_changed = false; return c; }
