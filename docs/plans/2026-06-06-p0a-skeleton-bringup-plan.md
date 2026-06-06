# P0-A Skeleton + Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A flashable PlatformIO `firmware/beacon/` that boots AXP2101 power -> CO5300 display, brings up CST92xx touch, runs LVGL 8.4 partial-render with a heap-floor-asserted buffer region, shows a safe-area test screen that reacts to touch, and freezes `SAFE_INSET` + `partitions.csv`.

**Architecture:** Single-responsibility modules under `src/` (config / hal / ui / util). HAL exposes plain init/read functions; `ui/lvgl_port` wires LVGL's flush callback to the display and its pointer indev to touch. `main.cpp` is wiring only, following the proven boot order in `tech.md` §3. LVGL handler runs on Core 1 (Arduino `loop()` default); watchdog enabled explicitly via `enableLoopWDT()` (off by default on arduino-esp32); render loop never blocks.

**Tech Stack:** PlatformIO + pioarduino (Arduino-ESP32 core 3.3.x), LVGL 8.4.0, GFX_Library_for_Arduino 1.6.4 (`Arduino_CO5300`/`Arduino_ESP32QSPI`), XPowersLib 0.2.6 (AXP2101), SensorLib 0.3.3 (present, not exercised in A). Target: Waveshare ESP32-S3-Touch-AMOLED-2.16 (466x466, 8MB OPI PSRAM, 16MB flash).

**Source spec:** `docs/specs/2026-06-06-p0a-skeleton-bringup-design.md`. Proven init reference: `docs/spikes/display-power/beacon_power_test/beacon_power_test.ino`.

**Conventions (apply to every task):** ASCII only (`=>` not arrows); `[BEACON]` logs via the logger, never `Serial.print` directly in modules; no magic numbers in logic (pins/consts in `config/`); no secrets in source; surgical diffs. **Commits are the user's** — each commit step stages files and asks the user to run the commit.

**Hardware verification note:** every task ends by flashing the board and reading serial @115200 (and/or the panel). "Expected" lines are what you must see. If you don't, stop and debug before moving on — do not proceed on an unproven step.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/beacon/platformio.ini` | platform/board/PSRAM/flash, pinned `lib_deps`, build flags, partitions |
| `firmware/beacon/partitions.csv` | FROZEN OTA layout (3MB x2 + nvs + littlefs + coredump) |
| `firmware/beacon/src/lv_conf.h` | LVGL 8.4 config (16-bit color + swap, custom tick, mem) |
| `firmware/beacon/src/config/pins.h` | all verified GPIO/I2C/QSPI pins |
| `firmware/beacon/src/config/layout.h` | `SCREEN_W/H`, `SAFE_INSET`, `CORNER_R` (frozen here) |
| `firmware/beacon/src/util/log.h` | `[BEACON]` level-gated logging macros |
| `firmware/beacon/src/hal/power.{h,cpp}` | AXP2101 rail init |
| `firmware/beacon/src/hal/display.{h,cpp}` | CO5300 init, brightness, bitmap blit, gfx accessor |
| `firmware/beacon/src/hal/touch.{h,cpp}` | CST92xx init + point read behind a minimal iface |
| `firmware/beacon/src/ui/lvgl_port.{h,cpp}` | LVGL buffers, flush cb, indev cb, heap-floor decision |
| `firmware/beacon/src/main.cpp` | boot sequence wiring + test screen |
| `firmware/beacon/secrets.example.h` | template (real `secrets.h` gitignored) — not used in A, placed for later chunks |

---

## Task 0: PlatformIO project skeleton (compiles + flashes a no-op)

**Files:**
- Create: `firmware/beacon/platformio.ini`
- Create: `firmware/beacon/partitions.csv`
- Create: `firmware/beacon/src/lv_conf.h`
- Create: `firmware/beacon/src/config/pins.h`
- Create: `firmware/beacon/src/config/layout.h`
- Create: `firmware/beacon/src/util/log.h`
- Create: `firmware/beacon/src/main.cpp`
- Create: `firmware/beacon/secrets.example.h`

- [ ] **Step 1: Create `platformio.ini`**

```ini
[env:beacon]
; pioarduino is the only platform shipping Arduino-ESP32 core 3.3.x (tech.md §5).
; Pin <TAG> to the release whose bundled core is 3.3.x; verified at boot (main.cpp logs it).
platform = https://github.com/pioarduino/platform-espressif32/releases/download/<TAG>/platform-espressif32.zip
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

- [ ] **Step 2: Create `partitions.csv` (frozen layout)**

```csv
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x5000
otadata,   data, ota,      0xe000,   0x2000
app0,      app,  ota_0,    0x10000,  0x300000
app1,      app,  ota_1,    0x310000, 0x300000
spiffs,    data, spiffs,   0x610000, 0x9E0000
coredump,  data, coredump, 0xff0000, 0x10000
```

- [ ] **Step 3: Create `src/config/pins.h`**

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

- [ ] **Step 4: Create `src/config/layout.h` (freeze artifact 1 — values locked in Task 1)**

```cpp
#pragma once
// Panel + safe-area geometry. SAFE_INSET / CORNER_R are FROZEN once the
// cyan-border test (Task 1) confirms them on hardware (FR-PLAT-9, DESIGN.md).
#define SCREEN_W   466
#define SCREEN_H   466
#define CORNER_R   90    // ~20% of 466; confirm on hardware
#define SAFE_INSET 40    // every screen lays out inside this inset; do not reduce
```

- [ ] **Step 5: Create `src/util/log.h`**

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

- [ ] **Step 6: Create `src/lv_conf.h`**

Copy LVGL 8.4.0's `lv_conf_template.h` into `firmware/beacon/src/lv_conf.h` (change the top `#if 0` to `#if 1`), then set exactly these values (leave the rest at template defaults):

```c
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1            /* CO5300 expects byte-swapped RGB565; flip if colors look wrong (Task 4) */
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

- [ ] **Step 7: Create `src/main.cpp` (no-op boot)**

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

- [ ] **Step 8: Create `secrets.example.h` (template for later chunks)**

```cpp
#pragma once
// Copy to secrets.h (gitignored) and fill in. Not used in P0-A.
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-pass"
```

- [ ] **Step 9: Build + flash + verify**

Run: `cd firmware/beacon && pio run -t upload && pio device monitor`
Expected serial:
```
[BEACON] I boot
[BEACON] I arduino-core=3.3.x sdk=v5.x...
[BEACON] I heap=... psram=8...   (psram in the 8,000,000+ range => OPI PSRAM is up)
[BEACON] I alive
```
If `arduino-core` is not `3.3.x`, fix the pioarduino `<TAG>` in `platformio.ini` before continuing. If `psram=0`, fix `memory_type=qio_opi`.

- [ ] **Step 10: Commit checkpoint (user runs the commit)**

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

- [ ] **Step 1: Create `src/hal/power.h`**

```cpp
#pragma once
// AXP2101 PMU. Rails MUST be enabled before the display (tech.md §3);
// ALDO2 = DSI_PWR_EN is the rail the stock Waveshare demo omits.
bool power_begin();   // true if the AXP2101 answered on I2C and rails were set
```

- [ ] **Step 2: Create `src/hal/power.cpp`**

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

- [ ] **Step 3: Create `src/hal/display.h`**

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

- [ ] **Step 4: Create `src/hal/display.cpp`**

```cpp
#include "display.h"
#include <Arduino_GFX_Library.h>
#include "config/pins.h"
#include "config/layout.h"
#include "util/log.h"

static Arduino_DataBus* s_bus = new Arduino_ESP32QSPI(
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
static Arduino_CO5300* s_gfx = new Arduino_CO5300(
    s_bus, PIN_LCD_RESET, 0 /*rotation*/, SCREEN_W, SCREEN_H, 0, 0, 0, 0);

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

- [ ] **Step 5: Replace `src/main.cpp` with the cyan-border / safe-area test**

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

- [ ] **Step 6: Build + flash + verify on the panel**

Run: `cd firmware/beacon && pio run -t upload && pio device monitor`
Expected serial:
```
[BEACON] I AXP2101 rails ALDO1-4 @3.3V
[BEACON] I CO5300 up 466x466
[BEACON] I cyan-border + safe-inset drawn; verify on panel
```
Expected panel: an **unbroken cyan rectangle touching all four edges** (confirms 466 working size) and a **red rectangle inset 40px** whose corners sit clear of the rounded arcs.

- [ ] **Step 7: Lock the freeze values**

If the cyan border is clipped on any edge, or the red inset corners fall inside an arc, adjust `CORNER_R` / `SAFE_INSET` in `config/layout.h` and re-flash until correct. Then the values in `layout.h` are **frozen** — add a one-line comment recording the panel-confirmed result, e.g. `// confirmed on hw 2026-06-06: border flush at 466, inset 40 clear`.

- [ ] **Step 8: Power-cycle test (the AXP regression)**

Press the PWR button to power off, then on. Expected: the display comes back (no black screen) — proves the ALDO2 rail init survives a PWR cycle (the bug the stock demo has).

- [ ] **Step 9: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/hal/power.h firmware/beacon/src/hal/power.cpp \
        firmware/beacon/src/hal/display.h firmware/beacon/src/hal/display.cpp \
        firmware/beacon/src/config/layout.h firmware/beacon/src/main.cpp
# Suggested message:
# feat(firmware): AXP2101 + CO5300 bring-up, lock SAFE_INSET (P0-A task 1)
```

---

## Task 2: Touch HAL (CST92xx) — the unproven item

**Files:**
- Create: `firmware/beacon/src/hal/touch.h`, `firmware/beacon/src/hal/touch.cpp`
- Modify: `firmware/beacon/src/main.cpp`

- [ ] **Step 1: Create `src/hal/touch.h`**

```cpp
#pragma once
#include <stdint.h>
// CST9220/CST9217 capacitive touch over I2C@0x5A, INT on PIN_TOUCH_INT.
bool touch_begin();                       // true if the controller answered on I2C
bool touch_read(int16_t* x, int16_t* y);  // true if a finger is currently down
```

- [ ] **Step 2: Vendor the Waveshare CST92xx driver, then implement `src/hal/touch.cpp`**

Get the touch driver from the Waveshare example pack (`github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16`, the `CST92xx`/`Touch` source). Confirm SensorLib does NOT cover CST9220/CST9217 before considering it (it doesn't, per `tech.md` §5). Implement the read against the CST9xx report format; **verify the register offsets below against the vendored driver** and adjust if they differ for this exact controller:

```cpp
#include "touch.h"
#include <Wire.h>
#include "config/pins.h"
#include "util/log.h"

// CST9xx report block: 0x00 status, 0x02 finger count, then per-point:
//   +0 hi nibble = event<<6 | x[11:8], +1 x[7:0], +2 y[11:8], +3 y[7:0]
// VERIFY these offsets against the vendored Waveshare driver for CST9220/CST9217.
static bool readReg(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(ADDR_TOUCH);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((int)ADDR_TOUCH, (int)len);
  for (uint8_t i = 0; i < got && i < len; i++) buf[i] = Wire.read();
  return got == len;
}

bool touch_begin() {
  pinMode(PIN_TOUCH_INT, INPUT);
  uint8_t probe;
  // Wire was already begun by power_begin(); just probe the address.
  Wire.beginTransmission(ADDR_TOUCH);
  bool ok = (Wire.endTransmission() == 0);
  LOGI("touch CST92xx @0x%02X: %s", ADDR_TOUCH, ok ? "present" : "ABSENT");
  (void)probe;
  return ok;
}

bool touch_read(int16_t* x, int16_t* y) {
  uint8_t b[7];
  if (!readReg(0x00, b, sizeof(b))) return false;
  uint8_t fingers = b[2] & 0x0F;
  if (fingers == 0) return false;
  *x = ((int16_t)(b[3] & 0x0F) << 8) | b[4];
  *y = ((int16_t)(b[5] & 0x0F) << 8) | b[6];
  return true;
}
```

- [ ] **Step 3: Add a touch probe to `main.cpp` setup() (after display_begin)**

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

- [ ] **Step 4: Build + flash + verify by touching the panel**

Run: `cd firmware/beacon && pio run -t upload && pio device monitor`
Expected serial: `[BEACON] I touch CST92xx @0x5A: present`, then on each tap `[BEACON] I touch x=<n> y=<n>`.
Expected panel: a red dot appears **under your fingertip**. Tap the four corners of the red SAFE_INSET rect and confirm coordinates are ~`(40,40)`..`(426,426)` and orientation matches (top-left tap reads small x,y). If x/y are swapped or mirrored, adjust the parse to match the panel's `0x36/0xA0` orientation and note it.

- [ ] **Step 5: Commit checkpoint (user runs the commit)**

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

- [ ] **Step 1: Create `src/ui/lvgl_port.h`**

```cpp
#pragma once
// LVGL 8.4 port: two partial draw buffers, flush -> display, indev <- touch.
// Buffer region (internal SRAM vs PSRAM) is chosen at boot per the heap floor.
bool lvgl_port_begin();   // call after display_begin() + touch_begin()
void lvgl_port_tick();    // pump from loop() on Core 1
```

- [ ] **Step 2: Create `src/ui/lvgl_port.cpp`**

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

- [ ] **Step 3: Slim `main.cpp` to wire LVGL (remove the raw-gfx loop)**

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

- [ ] **Step 4: Build + flash + verify**

Run: `cd firmware/beacon && pio run -t upload && pio device monitor`
Expected serial:
```
[BEACON] I lvgl buffers in internal-SRAM (2x 43804 B); free internal heap=<n> floor=61440
[BEACON] I setup done
```
`<n>` must be `>= 61440` (60KB). If the line says `PSRAM`, the fallback engaged — that is acceptable per spec, just note it. Panel shows a blank (black) LVGL screen — no crash, `alive` loop steady.

- [ ] **Step 5: Commit checkpoint (user runs the commit)**

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

- [ ] **Step 1: Create `src/ui/test_screen.h`**

```cpp
#pragma once
// Throwaway P0-A verification screen: proves LVGL render + touch indev + SAFE_INSET.
// Replaced by the real carousel in Chunk C.
void test_screen_show();
```

- [ ] **Step 2: Create `src/ui/test_screen.cpp`**

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

- [ ] **Step 3: Call it from `main.cpp` setup() after `lvgl_port_begin()`**

Add `#include "ui/test_screen.h"` and, right after the successful `lvgl_port_begin()`:

```cpp
  test_screen_show();
```

- [ ] **Step 4: Build + flash + verify**

Run: `cd firmware/beacon && pio run -t upload && pio device monitor`
Expected panel: white "BEACON P0-A" label near the top inside a faint bordered box; a "TAP ME" button centered. Tapping it increments "taps: N" and logs `[BEACON] I button tap N`. Touch-to-visual update feels immediate (< 100 ms, FR target). If text/colors look wrong-colored, toggle `LV_COLOR_16_SWAP` in `lv_conf.h`.

- [ ] **Step 5: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/ui/test_screen.h firmware/beacon/src/ui/test_screen.cpp firmware/beacon/src/main.cpp
# Suggested message:
# feat(firmware): LVGL safe-area test screen with touch (P0-A task 4)
```

---

## Task 5: Freeze partitions + boot-time + final acceptance

**Files:**
- Modify: `firmware/beacon/src/main.cpp` (boot timestamp log)

- [ ] **Step 1: Add boot-time instrumentation to `main.cpp`**

At the very end of `setup()`:

```cpp
  LOGI("boot-to-render %lu ms", (unsigned long)millis());
```

(`millis()` at end of setup approximates reset -> first rendered screen since the test screen is built in setup before the first `lv_timer_handler`.)

- [ ] **Step 2: Clean build from scratch with the frozen partition table**

Run:
```bash
cd firmware/beacon
pio run -t erase            # one-time: rewrites a 16MB-consistent bootloader + table
pio run -t upload && pio device monitor
```
Expected: boots cleanly from `ota_0`; no `partition exceeds flash chip size` error (the spike gotcha). Confirm the table:
```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --port <PORT> read_flash 0x8000 0xc00 ptable.bin
pio pkg exec -p framework-arduinoespressif32 -- gen_esp32part.py ptable.bin
```
Expected: `app0`/`app1` ota slots + `nvs` + `spiffs` + `coredump` exactly as `partitions.csv`. The table is now **frozen** (FR-PLAT-9).

- [ ] **Step 3: Run the full Chunk-A acceptance checklist (spec §8)**

Verify each on hardware and record the serial evidence:
- [ ] AXP2101 -> CO5300 boots; **PWR power-cycle** brings the panel back (no black screen).
- [ ] Cyan border flush to all edges; SAFE_INSET inset clear of arcs; `layout.h` frozen.
- [ ] Touch coords correct + correctly oriented across the panel.
- [ ] Serial logs buffer region + **min free internal heap >= 60 KB** (or PSRAM fallback noted).
- [ ] Test-screen button responds < 100 ms.
- [ ] `boot-to-render` log is **< 4000 ms**.
- [ ] Both OTA slots present; boots from `ota_0`.
- [ ] Clean build against pinned `tech.md` §5 versions; `arduino-core=3.3.x` in log; no secrets in source.

- [ ] **Step 4: Update README roadmap note (optional, surgical)**

If desired, leave the P0 checkbox unchecked (P0 = A+B+C+D) but you may add a sub-note that P0-A foundation/bring-up is complete. Keep it one line; do not restate history.

- [ ] **Step 5: Commit checkpoint (user runs the commit)**

```bash
git add firmware/beacon/src/main.cpp
# Suggested message:
# chore(firmware): boot-time instrumentation; freeze partitions (P0-A task 5)
```

---

## Self-Review (author checklist — done)

- **Spec coverage:** project skeleton/pioarduino/pinned libs (T0) · partitions freeze (T0 create, T5 verify) · power+display port (T1) · SAFE_INSET lock (T1) · touch CST92xx (T2) · LVGL partial buffers + heap-floor decision+log (T3) · safe-area test screen + touch indev (T4) · boot<4s + acceptance §8 (T5) · `[BEACON]` logger (T0) · core/task model: LVGL on Core 1 via `loop()` (Arduino `loop` runs on Core 1 by default) + watchdog via explicit `enableLoopWDT()` in T3 (arduino-esp32 leaves the loop WDT OFF by default) — covered; **note:** if a dedicated Core-1 LVGL task is wanted later it lands in Chunk C. All spec §3-§8 items map to a task.
- **Placeholder scan:** the only `<TAG>`/`<PORT>` tokens are genuine user-supplied values (pioarduino release tag; serial port), each with explicit resolution instructions — not lazy placeholders. The CST92xx register offsets carry an explicit "verify against vendored driver" instruction because the exact map is vendor-sourced, not inventable.
- **Type consistency:** `touch_read(int16_t*, int16_t*)`, `display_draw_bitmap(...)`, `display_gfx()`, `lvgl_port_begin()/tick()`, `power_begin()`, `display_begin()` are used identically across T1-T4.
