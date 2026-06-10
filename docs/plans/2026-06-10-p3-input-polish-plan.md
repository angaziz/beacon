# P3 — Input polish (IMU motion + richer touch + idle dim/sleep) — Implementation Plan

> Branch: `feat/p3-input-polish`. Authority: `prd.md` §5.1 (FR-PLAT-5/6/7) + §7, `DESIGN.md` §"Navigation".
> Stop point: firmware compiles clean (`pio run -e beacon`) + host tests pass (`pio test -e native`).
> On-hardware acceptance (the gesture/sleep behaviours) is the owner's validation step, same as P0–P2.

## Status — code complete, host-tested; on-hardware validation pending

The P3 input layer is implemented against the existing HAL/UI seams and the SensorLib QMI8658 driver
already vendored in `platformio.ini`. The pure decision logic (idle power policy, motion-gesture
detector) is split into Arduino/LVGL-free modules and host-tested under `[env:native]`, matching the
`carousel_nav.h` + `test_carousel` convention. The device-coupled parts (IMU read, display sleep,
LVGL gesture/long-press wiring) compile but need the board to confirm thresholds and the QMI8658 I2C
address — captured as the **on-hardware bring-up** checklist at the bottom.

## 0. BLUF

Three FRs, one input subsystem:

- **FR-PLAT-7 (idle dim -> sleep, wake on touch)** — the substrate motion-wake needs. Was deferred
  from P0; completed here because FR-PLAT-6's "raise/flick = wake display" is meaningless without a
  sleep state to wake from.
- **FR-PLAT-5 (richer touch)** — `swipe-down` = quick-brightness overlay; `long-press` = per-screen
  context action (force-refresh the device-direct data plane on Home/Finance; no-op elsewhere).
- **FR-PLAT-6 (IMU)** — `raise/flick` = wake; `shake` = dismiss the open overlay / exit a subview
  (per `DESIGN.md`: no carousel back-stack, so shake is the universal "back").

## 1. Module layout (new)

| File | Plane | Tested |
|---|---|---|
| `core/idle_policy.h` | pure (header-only, like `carousel_nav.h`) | `test/test_idle_policy` |
| `core/motion_gesture.{h,cpp}` | pure (no Arduino/LVGL) | `test/test_motion_gesture` |
| `hal/imu.{h,cpp}` | device (SensorLib QMI8658 on shared Wire) | on-hardware |
| `ui/brightness_overlay.{h,cpp}` | device (LVGL, modelled on `pair_overlay`) | on-hardware |
| `ui/input.{h,cpp}` | device (orchestrator, run from `loop()`) | on-hardware |

Touched: `config/pins.h` (`ADDR_IMU`), `hal/display.{h,cpp}` (`display_set_on`), `ui/lvgl_port.cpp`
(wake-touch gate), `ui/theme_panel.*` + `ui/wifi_panel.*` (public `*_close`), `core/fetch_task.*`
(`fetch_task_refresh_now`), `main.cpp` (`input_init`/`input_service`), `platformio.ini`
(native `build_src_filter` += motion_gesture), `README.md` + `prd.md` (roadmap tick).

## 2. Idle dim -> sleep (FR-PLAT-7)

`core/idle_policy.h` — pure mapping `idle_power_for(inactive_ms, dim_ms, sleep_ms) -> {AWAKE,DIM,ASLEEP}`.
The input controller reads `lv_disp_get_inactive_time(NULL)` each service tick and applies:

- `AWAKE`  -> restore persisted brightness, panel on.
- `DIM`    -> brightness lowered to `IDLE_DIM_LEVEL` (panel still on, content readable).
- `ASLEEP` -> `display_set_on(false)` (DCS 0x28); pixels off.

Thresholds: dim at 30 s, sleep at 300 s (matches the Settings "Sleep 5 min" row; editing it is FR-SET-5,
out of P3 scope). Wake resets inactivity via `lv_disp_trig_activity(NULL)`.

**Wake-on-touch without actuating a widget:** while `ASLEEP`, `indev_read_cb` reports `RELEASED`
(so the waking tap hits no button) but routes a detected finger to `input_note_wake_touch()`. Because
the indev reports no press while asleep, LVGL inactivity does NOT auto-reset — the controller wakes
explicitly. When merely `DIM` the panel is on and touch passes through normally (touch -> activity ->
AWAKE -> brightness restored).

## 3. Touch gestures (FR-PLAT-5)

Bound to `lv_scr_act()` (the screen), where LVGL actually delivers both events: gestures bubble
page->pager->screen via the default `GESTURE_BUBBLE`, and the pages/pager carry `EVENT_BUBBLE` so a
page long-press climbs to the screen too (the pager itself receives neither — see carousel.cpp):

- `LV_EVENT_GESTURE` + `lv_indev_get_gesture_dir() == LV_DIR_BOTTOM` -> `brightness_overlay_open()`.
  (The pager scrolls horizontal-only, so a vertical drag surfaces as a gesture, not a scroll.)
- `LV_EVENT_LONG_PRESSED` -> the active screen's context action. Added as an optional
  `screen_module_t.context_action` fn-pointer (NULL = none). Wired: Home/Finance ->
  `fetch_task_refresh_now()` (re-fetch their source now); hub-plane + Settings -> NULL.

`brightness_overlay` is a centred card (modelled on `pair_overlay`) with a slider bound to
`display_brightness` + `nvs_set_brightness`; auto-closes after a short idle, on tap-out, or on shake.

## 4. IMU gestures (FR-PLAT-6)

`hal/imu.cpp` brings up the QMI8658 (accel only: 4 G, 125 Hz, LPF) on the shared Wire bus — all
Core-1 I2C traffic (touch read, RTC write, IMU poll) is already serialized on Core-1, so no new bus
race. `input_service()` polls accel ~every 60 ms and feeds magnitude (milli-g) + timestamp to the
pure detector:

- `core/motion_gesture.cpp` — threshold state machine. A sharp jerk (|Δ|mag| past `WAKE_JERK`) =>
  `MOTION_WAKE`; >= N jerk reversals inside a window => `MOTION_SHAKE` (with refractory).
- Routing by display state: not-`AWAKE` + any motion -> wake; `AWAKE` + shake -> dismiss topmost
  overlay (`brightness_overlay` -> `theme_panel` -> `wifi_panel`); `pair_overlay` is left alone
  (it is pairing-state-driven and would just re-show).

## 5. Verification

- [ ] `pio test -e native` green (idle_policy + motion_gesture tables, plus existing suites).
- [ ] `pio run -e beacon` compiles clean; size guard < 85 %.
- [ ] Reviewer subagent pass on the diff.

### On-hardware bring-up (owner) — PENDING

- [ ] QMI8658 answers on I2C at `ADDR_IMU` (0x6B default; flip to 0x6A if `imu_begin` logs a miss).
- [ ] Idle: panel dims at 30 s, sleeps at 5 min; a touch wakes it without triggering the control under
      the finger.
- [ ] Flick/raise wakes from sleep; shake closes an open overlay.
- [ ] Swipe-down opens quick-brightness; the slider changes backlight and persists across reboot.
- [ ] Long-press on Home/Finance forces a visible re-fetch.
- [ ] Tune `WAKE_JERK` / shake window/threshold constants if gestures over- or under-trigger.
