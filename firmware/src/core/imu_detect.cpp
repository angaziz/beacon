#include "core/imu_detect.h"
#include <math.h>

// Tunables (refine on hardware):
#define SHAKE_G        1.8f    // |a| deviation from 1g that counts as a jolt
#define SHAKE_HITS     3       // jolts within the window => shake
#define SHAKE_WIN_MS   400
#define RAISE_DZ       0.5f    // z drop between consecutive samples => raise (tilt up)
#define RAISE_MAX_DT   200     // only if samples are close in time (a deliberate motion)

static float    s_lastz;
static uint32_t s_lastt;
static int      s_hits;
static uint32_t s_first_hit;
static uint8_t  s_pending;
static bool     s_have_last;

void imu_detect_reset(void) {
  s_lastz = 1.0f; s_lastt = 0; s_hits = 0; s_first_hit = 0; s_pending = 0; s_have_last = false;
}

void imu_detect_feed(float ax, float ay, float az, uint32_t t_ms) {
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  if (fabsf(mag - 1.0f) >= SHAKE_G) {
    if (s_hits == 0 || (t_ms - s_first_hit) > SHAKE_WIN_MS) { s_hits = 1; s_first_hit = t_ms; }
    else s_hits++;
    if (s_hits >= SHAKE_HITS) { s_pending |= IMU_SHAKE; s_hits = 0; }
  }
  if (s_have_last && (t_ms - s_lastt) <= RAISE_MAX_DT && (s_lastz - az) >= RAISE_DZ) {
    s_pending |= IMU_RAISE;
  }
  s_lastz = az; s_lastt = t_ms; s_have_last = true;
}

uint8_t imu_detect_poll(void) {
  uint8_t e = s_pending; s_pending = 0; return e;
}
