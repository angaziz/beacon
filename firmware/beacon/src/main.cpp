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
  // Outer cyan border: with the correct offset it should hug all 4 edges evenly.
  g->drawRect(0, 0, SCREEN_W, SCREEN_H, COL_CYAN);
  g->drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, COL_CYAN);
  // SAFE_INSET rectangle: nothing inside it may be clipped by a corner arc.
  g->drawRect(SAFE_INSET, SAFE_INSET, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET, COL_RED);
  g->setTextColor(COL_CYAN); g->setTextSize(2);
  g->setCursor(SAFE_INSET + 6, SAFE_INSET + 6); g->print("SAFE 40");
  LOGI("cyan-border + safe-inset drawn; verify on panel");
}

void loop() { delay(50); }
