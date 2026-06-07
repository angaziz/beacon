# P0-A Skeleton + Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Status:** P0-A is EXECUTED and hardware-verified on the Waveshare ESP32-S3-Touch-AMOLED-2.16. `firmware/beacon/` + `firmware/beacon/README.md` are the source of truth for the final code; this plan reflects the as-built approach.

**Goal:** A flashable PlatformIO `firmware/beacon/` that boots AXP2101 power -> CO5300 display, brings up CST92xx touch, runs LVGL 8.4 partial-render with a heap-floor-asserted buffer region, shows a safe-area test screen that reacts to touch, and freezes `SAFE_INSET` + `partitions.csv`.

**Architecture:** Single-responsibility modules under `src/` (config / hal / ui / util). HAL exposes plain init/read functions; `ui/lvgl_port` wires LVGL's flush callback to the display and its pointer indev to touch. `main.cpp` is wiring only, following the proven boot order in `tech.md` §3. LVGL handler runs on Core 1 (Arduino `loop()` default); watchdog enabled explicitly via `enableLoopWDT()` (off by default on arduino-esp32); render loop never blocks.

**Tech Stack:** PlatformIO + pioarduino (Arduino-ESP32 core 3.3.5), LVGL 8.4.0, GFX_Library_for_Arduino 1.6.4 (`Arduino_CO5300`/`Arduino_ESP32QSPI`), XPowersLib 0.2.6 (AXP2101), SensorLib 0.3.3 (`TouchDrvCST92xx`). Target: Waveshare ESP32-S3-Touch-AMOLED-2.16 (466x466, 8MB OPI PSRAM, 16MB flash).

**Source spec:** `docs/specs/2026-06-06-p0a-skeleton-bringup-design.md`. Proven init reference: `docs/spikes/display-power/beacon_power_test/beacon_power_test.ino`.

**Conventions (apply to every task):** ASCII only (`=>` not arrows); `[BEACON]` logs via the logger, never `Serial.print` directly in modules; no magic numbers in logic (pins/consts in `config/`); no secrets in source; surgical diffs. **Commits are the user's** — each commit step stages files and asks the user to run the commit.

**Hardware verification note:** every task ends by flashing the board and reading serial @115200 (and/or the panel). "Expected" lines are what you must see. If you don't, stop and debug before moving on — do not proceed on an unproven step.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/beacon/platformio.ini` | platform/board/PSRAM/flash, pinned `lib_deps`, build flags, partitions |
| `firmware/beacon/partitions.csv` | FROZEN OTA layout (3MB x2 + nvs + spiffs + coredump) |
| `firmware/beacon/src/lv_conf.h` | LVGL 8.4 config (16-bit color, no byte-swap, custom tick, mem) |
| `firmware/beacon/src/config/pins.h` | all verified GPIO/I2C/QSPI pins |
| `firmware/beacon/src/config/layout.h` | `SCREEN_W/H`, `SAFE_INSET`, `CORNER_R`, GRAM offsets (frozen here) |
| `firmware/beacon/src/util/log.h` | `[BEACON]` level-gated logging macros |
| `firmware/beacon/src/hal/power.{h,cpp}` | AXP2101 rail init |
| `firmware/beacon/src/hal/display.{h,cpp}` | CO5300 init, brightness, bitmap blit, gfx accessor |
| `firmware/beacon/src/hal/touch.{h,cpp}` | CST92xx init + point read behind a minimal iface |
| `firmware/beacon/src/ui/lvgl_port.{h,cpp}` | LVGL buffers, flush cb, rounder cb, indev cb, heap-floor decision |
| `firmware/beacon/src/main.cpp` | boot sequence wiring + test screen |
| `firmware/beacon/secrets.example.h` | template (real `secrets.h` gitignored) — not used in A, placed for later chunks |

---

## Task 0: Toolchain + PlatformIO project skeleton (compiles + flashes a no-op)

**Files:**
- Create: `firmware/beacon/platformio.ini`
- Create: `firmware/beacon/partitions.csv`
- Create: `firmware/beacon/src/lv_conf.h`
- Create: `firmware/beacon/src/config/pins.h`
- Create: `firmware/beacon/src/config/layout.h`
- Create: `firmware/beacon/src/util/log.h`
- Create: `firmware/beacon/src/main.cpp`
- Create: `firmware/beacon/secrets.example.h`

- [x] **Step 0: Toolchain prerequisite — PlatformIO under Python 3.13 in a venv**

PlatformIO must run under Python 3.13 from a dedicated venv at `~/.beacon-pio`. pioarduino `55.03.35`'s builder requires Python 3.10-3.13: Homebrew's `platformio` formula runs on Python 3.14 (rejected by the builder) and Xcode's bundled Python 3.9 is rejected by PlatformIO itself. The venv also needs `pyyaml` (the pioarduino builder imports it). Set it up once:

```bash
brew install python@3.13
/opt/homebrew/opt/python@3.13/bin/python3.13 -m venv ~/.beacon-pio
~/.beacon-pio/bin/pip install -U pip platformio pyyaml
```

Use `~/.beacon-pio/bin/pio` for every build/flash/monitor command in this plan. Full rationale and the pinned version matrix live in `firmware/beacon/README.md`.

- [x] **Step 1: Create `platformio.ini`**

```ini
[env:beacon]
; pioarduino is the only platform shipping Arduino-ESP32 core 3.3.x (tech.md §5).
; 55.03.35 == Arduino v3.3.5 / ESP-IDF v5.5.1. Verified at boot (main.cpp logs the core version).
; PIN RATIONALE: core 3.3.6+ changed spiFrequencyToClockDiv() to a 2-arg signature, which
; GFX_Library_for_Arduino 1.6.4/1.6.5 do not yet handle (their SPI databus files fail to compile).
; 3.3.5 is the LAST 3.3.x with the 1-arg signature => newest core that keeps the tech.md §5
; GFX 1.6.4 pin compiling clean. Do not bump past 55.03.35 until GFX gains 2-arg support.
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.35/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino

board_build.arduino.memory_type = qio_opi   ; OPI PSRAM
board_upload.flash_size = 16MB
board_build.flash_mode = qio
board_build.f_flash = 80000000L
board_build.partitions = partitions.csv
board_build.filesystem = littlefs

monitor_speed = 115200
monitor_filters = esp32_exception_decoder

build_flags =
  -DBOARD_HAS_PSRAM
  -DARDUINO_USB_CDC_ON_BOOT=1
  -DCORE_DEBUG_LEVEL=3
  -DLV_CONF_INCLUDE_SIMPLE
  -I src
  ; NOTE: do NOT add -DLV_CONF_PATH — LVGL 8.4 double-stringifies it and the include breaks.
  ; LV_CONF_INCLUDE_SIMPLE makes LVGL do #include "lv_conf.h", found via -I src.

lib_deps =
  lvgl/lvgl@8.4.0
  moononournation/GFX Library for Arduino@1.6.4
  lewisxhe/XPowersLib@0.2.6
  lewisxhe/SensorLib@0.3.3
```

- [x] **Step 2: Create `partitions.csv` (frozen layout)**

```csv
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x5000
otadata,   data, ota,      0xe000,   0x2000
app0,      app,  ota_0,    0x10000,  0x300000
app1,      app,  ota_1,    0x310000, 0x300000
spiffs,    data, spiffs,   0x610000, 0x9E0000
coredump,  data, coredump, 0xff0000, 0x10000
```

- [x] **Step 3: Create `src/config/pins.h`**

```cpp
#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-2.16 — verified pins (docs/spikes, tech.md §2).

// QSPI display
#define PIN_LCD_SDIO0 4
#define PIN_LCD_SDIO1 5
#define PIN_LCD_SDIO2 6
#define PIN_LCD_SDIO3 7
#define PIN_LCD_SCLK  38
#define PIN_LCD_RESET 2
#define PIN_LCD_CS    12

// I2C bus (AXP2101, touch, IMU, RTC share it)
#define PIN_IIC_SDA 15
#define PIN_IIC_SCL 14

// Touch
#define PIN_TOUCH_INT 11
#define ADDR_TOUCH    0x5A
```

- [x] **Step 4: Create `src/config/layout.h` (freeze artifact 1 — values locked in Task 1)**

```cpp
#pragma once
// Panel + safe-area geometry. SAFE_INSET / CORNER_R are FROZEN once the
// cyan-border test (Task 1) confirms them on hardware (FR-PLAT-9, DESIGN.md).
#define SCREEN_W   466
#define SCREEN_H   466
#define CORNER_R   90    // ~20% of 466; confirm on hardware
#define SAFE_INSET 40    // every screen lays out inside this inset; do not reduce

// CO5300 visible-window offset inside its 480x480 GRAM. Centered margin = (480-466)/2 = 7,
// but the CO5300 requires EVEN window coordinates (odd CASET/PASET corrupt partial redraws),
// and the LVGL rounder snaps logical coords to even -> GRAM start = logical + offset must stay even,
// so the offset must be even. 8 is the nearest even to the centered 7 (<=1px asymmetry, immaterial).
// Measured on hardware (P0-A): offset 0 lost top-left, 14 lost bottom-right; 8 centers + stays even.
#define LCD_X_OFFSET 8
#define LCD_Y_OFFSET 8
```

- [x] **Step 5: Create `src/util/log.h`**

```cpp
#pragma once
#include <Arduino.h>
// [BEACON] level-gated serial logging. Never log secrets (tech.md §8/§9).
// Levels: 0=off 1=err 2=warn 3=info. Compile-time gate via BEACON_LOG_LEVEL.
#ifndef BEACON_LOG_LEVEL
#define BEACON_LOG_LEVEL 3
#endif
#define LOG_AT(lvl, tag, fmt, ...) \
  do { if (BEACON_LOG_LEVEL >= (lvl)) Serial.printf("[BEACON] " tag " " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) LOG_AT(1, "E", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_AT(2, "W", fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_AT(3, "I", fmt, ##__VA_ARGS__)
```

- [x] **Step 6: Create `src/lv_conf.h`**

Copy LVGL 8.4.0's `lv_conf_template.h` into `firmware/beacon/src/lv_conf.h` (change the top `#if 0` to `#if 1`), then set exactly these values (leave the rest at template defaults):

```c
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0            /* paired with GFX draw16bitRGBBitmap (non-swapping), per Waveshare's proven LVGL config */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)     /* LVGL object pool (internal heap) */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1       /* used by the Task 4 test screen */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
```

- [x] **Step 7: Create `src/main.cpp` (no-op boot)**

```cpp
#include <Arduino.h>
#include "util/log.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot");
  LOGI("arduino-core=%s sdk=%s", ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion());
  LOGI("heap=%u psram=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 1000) { t = millis(); LOGI("alive"); }
  delay(5);
}
```

- [x] **Step 8: Create `secrets.example.h` (template for later chunks)**

```cpp
#pragma once
// Copy to secrets.h (gitignored) and fill in. Not used in P0-A.
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-pass"
```

- [x] **Step 9: Build + flash + verify**

Run: `cd firmware/beacon && ~/.beacon-pio/bin/pio run -t upload && ~/.beacon-pio/bin/pio device monitor`
Expected serial:
```
[BEACON] I boot
[BEACON] I arduino-core=3.3.5 sdk=v5.5.1...
[BEACON] I heap=... psram=8...   (psram in the 8,000,000+ range => OPI PSRAM is up)
[BEACON] I alive
```
If `arduino-core` is not `3.3.5`, the pioarduino platform pin is wrong in `platformio.ini`. If `psram=0`, fix `memory_type=qio_opi`.

- [x] **Step 10: Commit checkpoint (user runs the commit)**

Stage, then ask the user to commit:
```bash
git add firmware/beacon/platformio.ini firmware/beacon/partitions.csv \
        firmware/beacon/src firmware/beacon/secrets.example.h
# Suggested message:
# chore(firmware): scaffold PlatformIO project, partitions, config, logger (P0-A task 0)
```

---

## Task 1: Power + display HAL + lock SAFE_INSET (freeze artifact 1)

**Files:**
- Create: `firmware/beacon/src/hal/power.h`, `firmware/beacon/src/hal/power.cpp`
- Create: `firmware/beacon/src/hal/display.h`, `firmware/beacon/src/hal/display.cpp`
- Modify: `firmware/beacon/src/main.cpp`

- [x] **Step 1: Create `src/hal/power.h`**

```cpp
#pragma once
// AXP2101 PMU. Rails MUST be enabled before the display (tech.md §3);
// ALDO2 = DSI_PWR_EN is the rail the stock Waveshare demo omits.
bool power_begin();   // true if the AXP2101 answered on I2C and rails were set
```

- [x] **Step 2: Create `src/hal/power.cpp`**

```cpp
#define XPOWERS_CHIP_AXP2101
#include "power.h"
#include <Wire.h>
#include <XPowersLib.h>
#include "config/pins.h"
#include "util/log.h"

static XPowersPMU s_pmu;

bool power_begin() {
  Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL);
  bool ok = s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_IIC_SDA, PIN_IIC_SCL);
  if (!ok) { LOGE("AXP2101 begin FAIL (check I2C)"); return false; }
  s_pmu.setDC1Voltage(3300);
  s_pmu.setALDO1Voltage(3300);
  s_pmu.setALDO2Voltage(3300);   // DSI_PWR_EN — critical for the 2.16 panel
  s_pmu.setALDO4Voltage(3300);
  s_pmu.enableALDO1();
  s_pmu.enableALDO2();
  s_pmu.enableALDO3();           // display rail
  s_pmu.enableALDO4();
  LOGI("AXP2101 rails ALDO1-4 @3.3V");
  return true;
}
```

- [x] **Step 3: Create `src/hal/display.h`**

```cpp
#pragma once
#include <stdint.h>
class Arduino_GFX;
// CO5300 AMOLED over QSPI. display_begin() assumes power_begin() already ran.
bool display_begin();
void display_brightness(uint8_t level);                 // raw DCS 0x51 (no GFX API in 1.6.4)
void display_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* px);
Arduino_GFX* display_gfx();                              // for the cyan-border test (Task 1)
```

- [x] **Step 4: Create `src/hal/display.cpp`**

The CO5300 needs a GRAM offset: visible 466 sits centered inside the panel's 480x480 GRAM, so the `Arduino_CO5300` constructor is passed `(…, 466, 466, 8, 8, 0, 0)` — i.e. `LCD_X_OFFSET = LCD_Y_OFFSET = 8` from `layout.h`. Rationale (see `layout.h` comment): measured offset 0 lost the top-left, 14 lost the bottom-right; the geometric center is 7, but the CO5300 requires EVEN window coordinates, so the offset is locked at 8 (nearest even to 7, <=1px asymmetry). `SAFE_INSET` is locked at 40 on hardware.

```cpp
#include "display.h"
#include <Arduino_GFX_Library.h>
#include "config/pins.h"
#include "config/layout.h"
#include "util/log.h"

static Arduino_DataBus* s_bus = new Arduino_ESP32QSPI(
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
static Arduino_CO5300* s_gfx = new Arduino_CO5300(
    s_bus, PIN_LCD_RESET, 0 /*rotation*/, SCREEN_W, SCREEN_H, LCD_X_OFFSET, LCD_Y_OFFSET, 0, 0);

bool display_begin() {
  if (!s_gfx->begin()) { LOGE("CO5300 begin FAIL"); return false; }
  s_bus->writeC8D8(0x36, 0xA0);   // MADCTL (Waveshare value)
  s_bus->writeC8D8(0x53, 0x20);   // enable brightness control
  s_bus->writeC8D8(0x51, 0xFF);   // brightness = max
  s_gfx->fillScreen(0x0000);
  LOGI("CO5300 up %dx%d", SCREEN_W, SCREEN_H);
  return true;
}

void display_brightness(uint8_t level) { s_bus->writeC8D8(0x51, level); }

void display_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* px) {
  s_gfx->draw16bitRGBBitmap(x, y, px, w, h);
}

Arduino_GFX* display_gfx() { return s_gfx; }
```

- [x] **Step 5: Replace `src/main.cpp` with the cyan-border / safe-area test**

```cpp
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "util/log.h"
#include "config/layout.h"
#include "hal/power.h"
#include "hal/display.h"

#define COL_CYAN 0x07FF
#define COL_RED  0xF800

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot — core=%s", ESP_ARDUINO_VERSION_STR);

  if (!power_begin())   { LOGE("halt: power"); return; }
  delay(120);
  if (!display_begin()) { LOGE("halt: display"); return; }

  Arduino_GFX* g = display_gfx();
  // Outer cyan border: if fully visible, working size is 466 (not 480).
  g->drawRect(0, 0, SCREEN_W, SCREEN_H, COL_CYAN);
  g->drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, COL_CYAN);
  // SAFE_INSET rectangle: nothing inside it may be clipped by a corner arc.
  g->drawRect(SAFE_INSET, SAFE_INSET, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET, COL_RED);
  g->setTextColor(COL_CYAN); g->setTextSize(2);
  g->setCursor(SAFE_INSET + 6, SAFE_INSET + 6); g->print("SAFE 40");
  LOGI("cyan-border + safe-inset drawn; verify on panel");
}

void loop() { delay(50); }
```

- [x] **Step 6: Build + flash + verify on the panel**

Run: `cd firmware/beacon && ~/.beacon-pio/bin/pio run -t upload && ~/.beacon-pio/bin/pio device monitor`
Expected serial:
```
[BEACON] I AXP2101 rails ALDO1-4 @3.3V
[BEACON] I CO5300 up 466x466
[BEACON] I cyan-border + safe-inset drawn; verify on panel
```
Expected panel: an **unbroken cyan rectangle touching all four edges** (confirms 466 working size, offset 8 centers it) and a **red rectangle inset 40px** whose corners sit clear of the rounded arcs.

- [x] **Step 7: Lock the freeze values**

The cyan border is flush on all four edges with `LCD_X_OFFSET = LCD_Y_OFFSET = 8` and the red inset corners clear the arcs at `SAFE_INSET = 40`. Those values in `layout.h` are now **frozen**, with the hardware-confirmed rationale recorded inline (offset 0 lost top-left, 14 lost bottom-right, 8 centers + stays even; inset 40 clear).

- [ ] **Step 8: Power-cycle test (the AXP regression)** (optional, pending)

Press the PWR button to power off, then on. Expected: the display comes back (no black screen) — proves the ALDO2 rail init survives a PWR cycle (the bug the stock demo has).

- [x] **Step 9: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/hal/power.h firmware/beacon/src/hal/power.cpp \
        firmware/beacon/src/hal/display.h firmware/beacon/src/hal/display.cpp \
        firmware/beacon/src/config/layout.h firmware/beacon/src/main.cpp
# Suggested message:
# feat(firmware): AXP2101 + CO5300 bring-up, lock SAFE_INSET (P0-A task 1)
```

---

## Task 2: Touch HAL (CST92xx via SensorLib)

**Files:**
- Create: `firmware/beacon/src/hal/touch.h`, `firmware/beacon/src/hal/touch.cpp`
- Modify: `firmware/beacon/src/main.cpp`

SensorLib 0.3.3 provides `TouchDrvCST92xx` (umbrella header `TouchDrvCSTXXX.hpp`), which is exactly the driver this panel needs — on hardware the chip is detected as CST9220. There is no need to vendor Waveshare's driver or hand-roll register reads. Key behaviors: `setPins(-1, PIN_TOUCH_INT)` with reset = -1 because TP_RST shares GPIO2 with LCD_RESET (already pulsed by `display_begin()`); `begin(Wire, 0x5A, sda, scl)`; `setMaxCoordinates(480, 480)`; `setSwapXY(true)`; `setMirrorXY(true, false)`; `getPoint` results scaled from the 480-wide sensor space down to the visible 466. Verified on hardware: taps track the fingertip with no axis flip.

- [x] **Step 1: Create `src/hal/touch.h`**

```cpp
#pragma once
#include <stdint.h>
// CST92xx capacitive touch over the shared I2C bus. touch_begin() assumes
// power_begin() already ran (Wire is up) and display_begin() already pulsed
// the shared reset line (see touch.cpp for why reset is left to the display).
bool touch_begin();                       // true if the controller answered on I2C
bool touch_read(int16_t* x, int16_t* y);  // true if a finger is down; coords in 0..SCREEN_W/H-1
```

- [x] **Step 2: Create `src/hal/touch.cpp`**

```cpp
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
```

- [x] **Step 3: Add a touch probe to `main.cpp` setup() (after display_begin)**

Insert after the `display_begin()` block in `setup()`:

```cpp
  #include "hal/touch.h"   // add with the other includes at top
  // ... inside setup(), after display drawing:
  touch_begin();
```

And add to `loop()`:

```cpp
void loop() {
  int16_t x, y;
  if (touch_read(&x, &y)) {
    LOGI("touch x=%d y=%d", x, y);
    display_gfx()->fillCircle(x, y, 6, COL_RED);   // visual confirmation
  }
  delay(15);
}
```

- [x] **Step 4: Build + flash + verify by touching the panel**

Run: `cd firmware/beacon && ~/.beacon-pio/bin/pio run -t upload && ~/.beacon-pio/bin/pio device monitor`
Expected: `touch_begin()` returns true (CST9220 detected), then on each tap `[BEACON] I touch x=<n> y=<n>`.
Expected panel: a red dot appears **under your fingertip**. Tap the four corners of the red SAFE_INSET rect and confirm coordinates are ~`(40,40)`..`(426,426)` and orientation matches (top-left tap reads small x,y). Verified on hardware: taps track, no axis flip with `setSwapXY(true)` + `setMirrorXY(true, false)`.

- [x] **Step 5: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/hal/touch.h firmware/beacon/src/hal/touch.cpp firmware/beacon/src/main.cpp
# Suggested message:
# feat(firmware): CST92xx touch bring-up with coord verification (P0-A task 2)
```

---

## Task 3: LVGL port (partial buffers + heap-floor decision)

**Files:**
- Create: `firmware/beacon/src/ui/lvgl_port.h`, `firmware/beacon/src/ui/lvgl_port.cpp`
- Modify: `firmware/beacon/src/main.cpp`

- [x] **Step 1: Create `src/ui/lvgl_port.h`**

```cpp
#pragma once
// LVGL 8.4 port: two partial draw buffers, flush -> display, indev <- touch.
// Buffer region (internal SRAM vs PSRAM) is chosen at boot per the heap floor.
bool lvgl_port_begin();   // call after display_begin() + touch_begin()
void lvgl_port_tick();    // pump from loop() on Core 1
```

- [x] **Step 2: Create `src/ui/lvgl_port.cpp`**

The display driver registers a `rounder_cb` alongside `flush_cb`: the CO5300 needs even-aligned window bounds, and odd partial-flush coordinates corrupt pixels, so the rounder snaps `x1/y1` down to even and `x2/y2` up to odd (keeping width/height even).

```cpp
#include "lvgl_port.h"
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "hal/display.h"
#include "hal/touch.h"
#include "config/layout.h"
#include "util/log.h"

static const uint32_t BUF_LINES  = 47;                       // ~1/10 of 466 (tech.md §6)
static const size_t   BUF_PX     = (size_t)SCREEN_W * BUF_LINES;
static const size_t   BUF_BYTES  = BUF_PX * sizeof(lv_color_t);
static const size_t   HEAP_FLOOR = 60u * 1024u;              // tech.md §8

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* px) {
  int32_t w = a->x2 - a->x1 + 1;
  int32_t h = a->y2 - a->y1 + 1;
  display_draw_bitmap(a->x1, a->y1, w, h, (uint16_t*)px);
  lv_disp_flush_ready(drv);
}

// CO5300 needs even-aligned window bounds; odd partial-flush coords corrupt pixels.
// Snap x1/y1 down to even and x2/y2 up to odd so width/height stay even (per Waveshare).
static void rounder_cb(lv_disp_drv_t* drv, lv_area_t* a) {
  if (a->x1 & 1) a->x1--;
  if (!(a->x2 & 1)) a->x2++;
  if (a->y1 & 1) a->y1--;
  if (!(a->y2 & 1)) a->y2++;
}

static void indev_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  int16_t x, y;
  if (touch_read(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x; data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

bool lvgl_port_begin() {
  lv_init();

  uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
  const char* region = "internal-SRAM";
  s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!s_buf1 || !s_buf2 || free_int < HEAP_FLOOR) {       // fall back to PSRAM
    if (s_buf1) heap_caps_free(s_buf1);
    if (s_buf2) heap_caps_free(s_buf2);
    caps = MALLOC_CAP_SPIRAM; region = "PSRAM";
    s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
    s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, caps);
  }
  if (!s_buf1 || !s_buf2) { LOGE("lvgl buffer alloc FAIL"); return false; }
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, BUF_PX);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W; disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = flush_cb; disp_drv.draw_buf = &s_draw_buf;
  disp_drv.rounder_cb = rounder_cb;   // even-align flush window (CO5300 requirement)
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&indev_drv);

  free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  LOGI("lvgl buffers in %s (2x %u B); free internal heap=%u floor=%u",
       region, (unsigned)BUF_BYTES, (unsigned)free_int, (unsigned)HEAP_FLOOR);
  if (free_int < HEAP_FLOOR) LOGW("below 60KB internal floor (Chunk-A baseline; remeasure at P2)");
  return true;
}

void lvgl_port_tick() { lv_timer_handler(); }
```

- [x] **Step 3: Slim `main.cpp` to wire LVGL (remove the raw-gfx loop)**

```cpp
#include <Arduino.h>
#include "util/log.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "ui/lvgl_port.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("boot — core=%s", ESP_ARDUINO_VERSION_STR);
  enableLoopWDT();   // tech.md §6: watchdog on (arduino-esp32 leaves the loop WDT OFF by default)
  if (!power_begin())   { LOGE("halt: power");   return; }
  delay(120);
  if (!display_begin()) { LOGE("halt: display"); return; }
  touch_begin();
  if (!lvgl_port_begin()) { LOGE("halt: lvgl"); return; }
  LOGI("setup done");
}

void loop() {
  lvgl_port_tick();
  delay(5);
}
```

- [x] **Step 4: Build + flash + verify**

Run: `cd firmware/beacon && ~/.beacon-pio/bin/pio run -t upload && ~/.beacon-pio/bin/pio device monitor`
Measured results on hardware: buffers landed in **internal SRAM** (both 47-line partial buffers), **free internal heap = 199 KB** (well above the 60 KB floor), and **boot-to-render = 1.12 s** (under the 4 s budget). Expected serial:
```
[BEACON] I lvgl buffers in internal-SRAM (2x 43804 B); free internal heap=199... floor=61440
[BEACON] I setup done
```
The panel shows a blank (black) LVGL screen — no crash, render loop steady.

- [x] **Step 5: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/ui/lvgl_port.h firmware/beacon/src/ui/lvgl_port.cpp firmware/beacon/src/main.cpp
# Suggested message:
# feat(firmware): LVGL 8.4 port, partial buffers, heap-floor decision (P0-A task 3)
```

---

## Task 4: Safe-area test screen reacting to touch

**Files:**
- Create: `firmware/beacon/src/ui/test_screen.h`, `firmware/beacon/src/ui/test_screen.cpp`
- Modify: `firmware/beacon/src/main.cpp`

- [x] **Step 1: Create `src/ui/test_screen.h`**

```cpp
#pragma once
// Throwaway P0-A verification screen: proves LVGL render + touch indev + SAFE_INSET.
// Replaced by the real carousel in Chunk C.
void test_screen_show();
```

- [x] **Step 2: Create `src/ui/test_screen.cpp`**

```cpp
#include "test_screen.h"
#include <lvgl.h>
#include "config/layout.h"
#include "util/log.h"

static lv_obj_t* s_label;
static int s_taps = 0;

static void btn_cb(lv_event_t* e) {
  s_taps++;
  lv_label_set_text_fmt(s_label, "taps: %d", s_taps);
  LOGI("button tap %d", s_taps);
}

void test_screen_show() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  // Safe-area container: everything lives inside SAFE_INSET.
  lv_obj_t* safe = lv_obj_create(scr);
  lv_obj_set_size(safe, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET);
  lv_obj_center(safe);
  lv_obj_set_style_bg_opa(safe, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(safe, 1, 0);
  lv_obj_set_style_border_color(safe, lv_color_hex(0x333333), 0);

  s_label = lv_label_create(safe);
  lv_label_set_text(s_label, "BEACON P0-A");
  lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
  lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t* btn = lv_btn_create(safe);
  lv_obj_set_size(btn, 200, 80);     // >= 64px touch target (DESIGN.md)
  lv_obj_center(btn);
  lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, "TAP ME");
  lv_obj_center(bl);
}
```

- [x] **Step 3: Call it from `main.cpp` setup() after `lvgl_port_begin()`**

Add `#include "ui/test_screen.h"` and, right after the successful `lvgl_port_begin()`:

```cpp
  test_screen_show();
```

- [x] **Step 4: Build + flash + verify**

Run: `cd firmware/beacon && ~/.beacon-pio/bin/pio run -t upload && ~/.beacon-pio/bin/pio device monitor`
Expected panel: white "BEACON P0-A" label near the top inside a faint bordered box; a "TAP ME" button centered. Tapping it increments "taps: N" and logs `[BEACON] I button tap N`. Touch-to-visual update feels immediate (< 100 ms, FR target). Colors render correct with `LV_COLOR_16_SWAP 0`.

- [x] **Step 5: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/ui/test_screen.h firmware/beacon/src/ui/test_screen.cpp firmware/beacon/src/main.cpp
# Suggested message:
# feat(firmware): LVGL safe-area test screen with touch (P0-A task 4)
```

---

## Task 5: Freeze partitions + boot-time + final acceptance

**Files:**
- Modify: `firmware/beacon/src/main.cpp` (boot timestamp log)

- [x] **Step 1: Add boot-time instrumentation to `main.cpp`**

The final `main.cpp` folds the boot-to-render log into the existing "setup done" line at the end of `setup()`:

```cpp
  LOGI("setup done; boot-to-render %lu ms", (unsigned long)millis());
```

(`millis()` at end of setup approximates reset -> first rendered screen since the test screen is built in setup before the first `lv_timer_handler`. Measured: 1.12 s.)

- [x] **Step 2: Clean build from scratch with the frozen partition table**

Run:
```bash
cd firmware/beacon
~/.beacon-pio/bin/pio run -t erase            # one-time: rewrites a 16MB-consistent bootloader + table
~/.beacon-pio/bin/pio run -t upload && ~/.beacon-pio/bin/pio device monitor
```
Boots cleanly from `ota_0`; no `partition exceeds flash chip size` error (the spike gotcha). The table was read back off the device and verified (`esptool` needs `~/.beacon-pio/bin/pip install esptool`; `gen_esp32part.py` decodes the dump):
```bash
~/.beacon-pio/bin/pip install esptool
~/.beacon-pio/bin/python -m esptool --port <PORT> read_flash 0x8000 0xc00 ptable.bin
~/.beacon-pio/bin/python ~/.platformio/packages/framework-arduinoespressif32/tools/gen_esp32part.py ptable.bin
```
Verified table on device: `nvs` 20K, `otadata` 8K, `app0`/`ota_0` 3M, `app1`/`ota_1` 3M, `spiffs` 10112K, `coredump` 64K — exactly as `partitions.csv`. The table is now **frozen** (FR-PLAT-9).

- [x] **Step 3: Run the full Chunk-A acceptance checklist (spec §8)**

Verified each on hardware and recorded the serial evidence:
- [x] AXP2101 -> CO5300 boots.
- [x] Cyan border flush to all edges; SAFE_INSET inset clear of arcs; `layout.h` frozen (offset 8, inset 40).
- [x] Touch coords correct + correctly oriented across the panel.
- [x] Serial logs buffer region (internal SRAM) + min free internal heap = 199 KB (>= 60 KB floor).
- [x] Test-screen button responds < 100 ms.
- [x] `boot-to-render` log = 1.12 s (< 4000 ms).
- [x] Both OTA slots present; boots from `ota_0`.
- [x] Clean build against pinned `tech.md` §5 versions; `arduino-core=3.3.5` in log; no secrets in source.
- [ ] **PWR power-cycle** brings the panel back (no black screen) — optional survival test, pending.

- [x] **Step 4: Update README roadmap note (optional, surgical)**

`firmware/beacon/README.md` documents the build/flash toolchain (pinned version matrix, venv setup) and is the source of truth for running the firmware.

- [x] **Step 5: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/main.cpp
# Suggested message:
# chore(firmware): boot-time instrumentation; freeze partitions (P0-A task 5)
```

---

## Self-Review (author checklist — done)

- **Spec coverage:** project skeleton/pioarduino/pinned libs (T0) · partitions freeze (T0 create, T5 verify) · power+display port (T1) · SAFE_INSET + GRAM offset lock (T1) · touch CST92xx via SensorLib (T2) · LVGL partial buffers + rounder_cb + heap-floor decision+log (T3) · safe-area test screen + touch indev (T4) · boot<4s + acceptance §8 (T5) · `[BEACON]` logger (T0) · core/task model: LVGL on Core 1 via `loop()` (Arduino `loop` runs on Core 1 by default) + watchdog via explicit `enableLoopWDT()` in T3 (arduino-esp32 leaves the loop WDT OFF by default) — covered; **note:** if a dedicated Core-1 LVGL task is wanted later it lands in Chunk C. All spec §3-§8 items map to a task.
- **Toolchain:** PlatformIO runs from the `~/.beacon-pio` Python 3.13 venv (pioarduino `55.03.35` requires Python 3.10-3.13; Homebrew's 3.14 and Xcode's 3.9 are both rejected); the venv carries `pyyaml` (builder import) and `esptool` (partition readback). Full matrix in `firmware/beacon/README.md`.
- **Type consistency:** `touch_read(int16_t*, int16_t*)`, `display_draw_bitmap(...)`, `display_gfx()`, `lvgl_port_begin()/tick()`, `power_begin()`, `display_begin()` are used identically across T1-T4.
