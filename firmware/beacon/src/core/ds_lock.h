#pragma once
// Thin mutex wrapper so datastore.cpp stays portable: std::mutex on the host test
// build (BEACON_NATIVE), a FreeRTOS mutex on device. Critical sections are pure
// struct copies (tech.md §6) — no I/O ever holds the lock.

#ifdef BEACON_NATIVE
  #include <mutex>
  typedef std::mutex ds_lock_t;
  static inline void ds_lock_init(ds_lock_t&) {}
  static inline void ds_lock_take(ds_lock_t& m) { m.lock(); }
  static inline void ds_lock_give(ds_lock_t& m) { m.unlock(); }
#else
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
  typedef SemaphoreHandle_t ds_lock_t;
  static inline void ds_lock_init(ds_lock_t& m) { m = xSemaphoreCreateMutex(); }
  static inline void ds_lock_take(ds_lock_t& m) { xSemaphoreTake(m, portMAX_DELAY); }
  static inline void ds_lock_give(ds_lock_t& m) { xSemaphoreGive(m); }
#endif
