#pragma once
#include "core/screen_state.h"   // data_err_t

// One weather fetch cycle (Open-Meteo): fetch -> parse -> publish to the DataStore weather record
// (ST_LIVE) or set ST_OFFLINE/ST_ERROR. Runs on the Core-0 fetch task only. Returns the result so the
// scheduler can pick a retry vs cadence delay.
#ifdef __cplusplus
extern "C" {
#endif
data_err_t fetch_weather(void);
#ifdef __cplusplus
}
#endif
