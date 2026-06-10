#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure, Arduino/LVGL-free formatters for the About panel. Kept here so the host
// (BEACON_NATIVE) unit tests can exercise the only logic worth testing.

// "AA:BB:CC:DD:EE:FF" -- out must hold >= 18 bytes.
void about_fmt_mac(const uint8_t mac[6], char out[18]);

// Minute resolution, two most-significant units, floored to "0m" under a minute:
// "0m" / "3m" / "3h 12m" / "2d 4h".
void about_fmt_uptime(uint32_t secs, char* out, size_t cap);

// Bytes -> "142 KB" (integer KB, truncated).
void about_fmt_heap_kb(uint32_t bytes, char* out, size_t cap);
