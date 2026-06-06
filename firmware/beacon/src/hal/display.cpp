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
  s_gfx->fillScreen(0x0000);
  LOGI("CO5300 up %dx%d", SCREEN_W, SCREEN_H);
  return true;
}

void display_brightness(uint8_t level) { s_bus->writeC8D8(0x51, level); }

void display_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* px) {
  s_gfx->draw16bitRGBBitmap(x, y, px, w, h);
}

Arduino_GFX* display_gfx() { return s_gfx; }
