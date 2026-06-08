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
