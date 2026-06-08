#pragma once
#include <stdint.h>
#include "core/screen_state.h"   // data_err_t

// One fetch cycle for ticker slot `idx` (dispatched by its configured source). Fetch -> parse ->
// compute change vs its change_basis -> publish (ST_LIVE) or set ST_OFFLINE/ST_ERROR on that slot
// only (the finance array is independently stateful). Core-0 fetch task only. Returns the result so
// the scheduler can pick a retry vs cadence delay.
#ifdef __cplusplus
extern "C" {
#endif
data_err_t fetch_finance(uint8_t idx);
#ifdef __cplusplus
}
#endif
