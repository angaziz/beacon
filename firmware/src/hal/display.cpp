#include "display.h"
#include <Arduino_GFX_Library.h>
#include "config/pins.h"
#include "config/layout.h"
#include "util/log.h"

static Arduino_DataBus* s_bus = new Arduino_ESP32QSPI(
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
static Arduino_CO5300* s_gfx = new Arduino_CO5300(
    s_bus, PIN_LCD_RESET, 0 /*rotation*/, SCREEN_W, SCREEN_H,
    LCD_X_OFFSET, LCD_Y_OFFSET, 0, 0);   // visible 466 window is offset in the 480 GRAM

bool display_begin() {
  if (!s_gfx->begin()) { LOGE("CO5300 begin FAIL"); return false; }
  s_bus->writeC8D8(0x36, 0xA0);   // MADCTL (Waveshare value)
  s_bus->writeC8D8(0x53, 0x20);   // enable brightness control
  s_bus->writeC8D8(0x51, 0xFF);   // brightness = max
  // Clear the FULL 480x480 GRAM, not just the offset-8 466 visible window: the unpainted edge
  // ring otherwise shows uninitialized (greenish) GRAM at boot. Offset-0 instance, no reset.
  static Arduino_CO5300 s_full(s_bus, GFX_NOT_DEFINED, 0, 480, 480, 0, 0, 0, 0);
  s_full.fillScreen(0x0000);
  s_gfx->fillScreen(0x0000);
  LOGI("CO5300 up %dx%d", SCREEN_W, SCREEN_H);
  return true;
}

void display_brightness(uint8_t level) { s_bus->writeC8D8(0x51, level); }

// Real panel sleep, not just brightness 0. Writing 0x51=0 blanks the pixels but leaves the CO5300
// scanning the full 480x480 GRAM continuously -- which is what the device did all night. 0x28 stops
// the scan-out, 0x10 drops the driver into its low-power state.
//
// The 120ms waits are the DCS-standard settling times around SLPIN/SLPOUT; skipping the one after
// 0x11 is the classic "panel stays dark until the next reboot" bug, so do not trim them. Note this
// touches only DCS state -- the AXP2101 rails stay up, so the boot-order rule in CLAUDE.md (power
// before display) is not in play here; a sleeping panel is not an unpowered one.
static bool s_asleep = false;

bool display_is_asleep() { return s_asleep; }

void display_sleep(bool on) {
  if (on == s_asleep) return;
  if (on) {
    s_bus->writeCommand(0x28);   // display off: stop scanning out GRAM
    s_bus->writeCommand(0x10);   // sleep in
    delay(120);
  } else {
    s_bus->writeCommand(0x11);   // sleep out
    delay(120);                  // mandatory before the panel accepts display-on
    s_bus->writeCommand(0x29);   // display on
    // Brightness control state is not guaranteed to survive the sleep cycle; re-arm it. The caller
    // sets the actual level right after, so 0x51 here only needs to not be left at the reset value.
    s_bus->writeC8D8(0x53, 0x20);
  }
  s_asleep = on;
  LOGI("panel %s", on ? "sleep (0x28+0x10)" : "wake (0x11+0x29)");
}

void display_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* px) {
  s_gfx->draw16bitRGBBitmap(x, y, px, w, h);
}

Arduino_GFX* display_gfx() { return s_gfx; }
