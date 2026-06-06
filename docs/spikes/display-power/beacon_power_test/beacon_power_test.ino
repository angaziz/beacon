// Beacon spike — AXP2101 power-on + display test
// Fixes the "black screen" by enabling the display rails the Waveshare
// 05 demo forgets to. Pins/values from Waveshare pin_config.h + the
// claude-desktop-buddy 2.16 power init (ALDO2 = DSI_PWR_EN is the key one).
//
// Board settings (Tools): ESP32S3 Dev Module, PSRAM: OPI PSRAM,
// Flash 16MB, USB CDC On Boot: Enabled. Upload, then open Serial @115200.

#define XPOWERS_CHIP_AXP2101
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <Arduino_GFX_Library.h>

// --- pins (Waveshare ESP32-S3-Touch-AMOLED-2.16, from pin_config.h) ---
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_RESET 2
#define LCD_CS 12
#define LCD_WIDTH 466
#define LCD_HEIGHT 466
#define IIC_SDA 15
#define IIC_SCL 14

// RGB565 colors (this GFX build doesn't predefine the named macros)
#define BLACK 0x0000
#define WHITE 0xFFFF
#define CYAN  0x07FF
#define RED   0xF800

XPowersPMU power;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[BEACON] boot");

  // 1) Power chip first — without ALDO2/ALDO3 the panel stays dark.
  Wire.begin(IIC_SDA, IIC_SCL);
  bool ok = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  Serial.printf("[BEACON] AXP2101 begin: %s\n", ok ? "OK" : "FAIL (check I2C)");
  if (ok) {
    power.setDC1Voltage(3300);
    power.setALDO1Voltage(3300);
    power.setALDO2Voltage(3300);   // DSI_PWR_EN — display power-enable
    power.setALDO4Voltage(3300);
    power.enableALDO1();
    power.enableALDO2();           // <-- the rail 05_LVGL_Widgets never turns on
    power.enableALDO3();           // display rail (idempotent)
    power.enableALDO4();
    Serial.println("[BEACON] AXP rails ALDO1-4 enabled @3.3V");
  }
  delay(120);

  // 2) Display
  Serial.println("[BEACON] gfx->begin()...");
  gfx->begin();
  bus->writeC8D8(0x36, 0xA0);      // MADCTL (Waveshare example value)
  bus->writeC8D8(0x53, 0x20);      // CTRL display: enable brightness control
  bus->writeC8D8(0x51, 0xFF);      // WRITE_DISPLAY_BRIGHTNESS = max (CO5300 / MIPI DCS)
  gfx->fillScreen(BLACK);

  // bright border so any panel offset is obvious (466 vs 480 question)
  gfx->drawRect(0, 0, LCD_WIDTH, LCD_HEIGHT, CYAN);
  gfx->drawRect(2, 2, LCD_WIDTH - 4, LCD_HEIGHT - 4, CYAN);

  gfx->setTextColor(WHITE);
  gfx->setTextSize(4);
  gfx->setCursor(70, 175);
  gfx->print("BEACON");
  gfx->setTextSize(2);
  gfx->setTextColor(CYAN);
  gfx->setCursor(70, 235);
  gfx->print("display alive");
  gfx->setTextColor(WHITE);
  gfx->setCursor(70, 270);
  gfx->print(LCD_WIDTH);
  gfx->print(" x ");
  gfx->print(LCD_HEIGHT);

  Serial.println("[BEACON] display drawn — you should see a cyan border + text");
}

void loop() {
  static uint32_t t = 0;
  static int n = 0;
  if (millis() - t > 1000) {
    t = millis();
    Serial.printf("[BEACON] alive %ds  heap=%u  psram=%u\n",
                  n, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    gfx->fillRect(LCD_WIDTH / 2 - 20, 320, 40, 40, (n % 2) ? RED : BLACK);  // pulsing block = it's running
    n++;
  }
}
