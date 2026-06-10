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

// IMU (QMI8658, 6-axis, shared I2C). 0x6B = address pad to GND (board default); 0x6A if tied to VDD.
#define ADDR_IMU 0x6B
