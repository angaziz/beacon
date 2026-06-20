// Beacon spike Step 2 — WiFi + BLE + HTTPS coexistence test
// Proves the #1 architectural risk: WiFi STA + TLS fetch + BLE advertising (Bluedroid)
// all running together on the ESP32-S3, with live + MINIMUM heap on screen.
// Built on the working AXP2101 + CO5300 init from beacon_power_test.
//
//   >>> EDIT WIFI_SSID / WIFI_PASS BELOW, then Upload. <<<
// Tools: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash 16MB, USB CDC On Boot: Enabled.

#define XPOWERS_CHIP_AXP2101
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// ============ EDIT THESE before uploading ============
const char* WIFI_SSID = "YOUR_SSID_HERE";
const char* WIFI_PASS = "YOUR_PASSWORD_HERE";
// =====================================================

// pins (Waveshare ESP32-S3-Touch-AMOLED-2.16)
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

#define BLACK 0x0000
#define WHITE 0xFFFF
#define CYAN  0x07FF
#define RED   0xF800
#define GREEN 0x07E0
#define YELLOW 0xFFE0
#define GREY  0x8410

XPowersPMU power;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

BLEServer* bleServer = nullptr;
String wxTemp = "--";
String httpStatus = "idle";
uint32_t lastFetch = 0;
uint32_t minHeap = 0xFFFFFFFF;

void fetchWeather() {
  WiFiClientSecure client;
#pragma message("SPIKE: setInsecure() disables TLS cert validation -- production uses setCACert()")
  client.setInsecure();  // SPIKE ONLY — production validates certs (see DESIGN.md)
  HTTPClient https;
  uint32_t t0 = millis();
  const char* url = "https://api.open-meteo.com/v1/forecast?latitude=-6.2&longitude=106.8&current=temperature_2m";
  if (https.begin(client, url)) {
    int code = https.GET();
    if (code == 200) {
      String p = https.getString();
      int c = p.indexOf("\"current\":");
      int i = p.indexOf("\"temperature_2m\":", c);
      if (i >= 0) {
        i += 17;
        int j = i;
        while (j < (int)p.length() && (isdigit(p[j]) || p[j] == '.' || p[j] == '-')) j++;
        wxTemp = p.substring(i, j);
      }
      httpStatus = "200 / " + String(millis() - t0) + "ms";
    } else {
      httpStatus = "HTTP " + String(code);
    }
    https.end();
  } else {
    httpStatus = "begin fail";
  }
  Serial.printf("[BEACON] weather temp=%s (%s)\n", wxTemp.c_str(), httpStatus.c_str());
}

void drawHUD() {
  uint32_t heap = ESP.getFreeHeap();
  if (heap < minHeap) minHeap = heap;
  int ble = bleServer ? bleServer->getConnectedCount() : 0;
  bool wifi = (WiFi.status() == WL_CONNECTED);

  gfx->fillScreen(BLACK);
  gfx->drawRect(0, 0, LCD_WIDTH, LCD_HEIGHT, GREY);
  gfx->setTextSize(3); gfx->setTextColor(CYAN); gfx->setCursor(22, 20); gfx->print("BEACON COEX");

  gfx->setTextSize(2);
  int y = 78;
  auto line = [&](const char* k, const String& v, uint16_t col) {
    gfx->setTextColor(GREY);  gfx->setCursor(22, y);  gfx->print(k);
    gfx->setTextColor(col);   gfx->setCursor(200, y); gfx->print(v);
    y += 32;
  };
  line("WiFi", wifi ? WiFi.localIP().toString() : String("connecting"), wifi ? GREEN : YELLOW);
  line("RSSI", wifi ? (String(WiFi.RSSI()) + " dBm") : String("-"), WHITE);
  line("BLE", ble > 0 ? ("connected " + String(ble)) : String("advertising"), ble > 0 ? GREEN : CYAN);
  line("WX temp", wxTemp + " C", WHITE);   // proves WiFi+TLS+parse end to end
  line("HTTP", httpStatus, WHITE);
  y += 12;
  line("heap", String(heap), heap > 120000 ? GREEN : RED);
  line("heap MIN", String(minHeap), minHeap > 100000 ? GREEN : RED);
  line("psram", String(ESP.getFreePsram()), WHITE);

  static bool hb = false; hb = !hb;   // heartbeat dot = loop is alive under load
  gfx->fillCircle(LCD_WIDTH - 38, 40, 10, hb ? RED : BLACK);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[BEACON] coex spike boot");

  // 1) Power + display (proven init)
  Wire.begin(IIC_SDA, IIC_SCL);
  bool ok = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (ok) {
    power.setDC1Voltage(3300);
    power.setALDO1Voltage(3300);
    power.setALDO2Voltage(3300);
    power.setALDO4Voltage(3300);
    power.enableALDO1(); power.enableALDO2(); power.enableALDO3(); power.enableALDO4();
  }
  Serial.printf("[BEACON] AXP2101: %s\n", ok ? "OK" : "FAIL");
  gfx->begin();
  bus->writeC8D8(0x36, 0xA0);
  bus->writeC8D8(0x53, 0x20);
  bus->writeC8D8(0x51, 0xFF);
  gfx->fillScreen(BLACK);

  // 2) WiFi (non-blocking; status shown on HUD)
  Serial.printf("[BEACON] WiFi -> %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // 3) BLE (Bluedroid — built into the core, version-matched to the controller)
  Serial.println("[BEACON] BLE init");
  BLEDevice::init("Beacon-Spike");
  bleServer = BLEDevice::createServer();
  BLEService* svc = bleServer->createService("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
  svc->createCharacteristic("6e400003-b5a3-f393-e0a9-e50e24dcca9e",
                            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
  adv->start();
  Serial.println("[BEACON] BLE advertising as 'Beacon-Spike'");
  Serial.printf("[BEACON] post-init heap=%u psram=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

void loop() {
  static uint32_t lastDraw = 0;

  if (WiFi.status() == WL_CONNECTED && millis() - lastFetch > 8000) {
    lastFetch = millis();
    fetchWeather();
  }
  if (millis() - lastDraw > 1000) {
    lastDraw = millis();
    drawHUD();
    Serial.printf("[BEACON] wifi=%d ble=%d heap=%u min=%u psram=%u temp=%s\n",
                  WiFi.status() == WL_CONNECTED,
                  bleServer ? bleServer->getConnectedCount() : 0,
                  (unsigned)ESP.getFreeHeap(), (unsigned)minHeap,
                  (unsigned)ESP.getFreePsram(), wxTemp.c_str());
  }
  delay(5);
}
