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

// Audio — ES8311 codec + I2S (device-only; not used in native/host builds)
#if !BEACON_NATIVE
#define AUDIO_I2S_MCLK   42
#define AUDIO_I2S_BCLK    9
#define AUDIO_I2S_WS     45
#define AUDIO_I2S_DOUT    8
#define AUDIO_PA_CTRL    46   // HIGH = amp unmuted
#define AUDIO_ES8311_ADDR 0x18
#endif
