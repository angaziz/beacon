#pragma once
#include "core/hublink.h"
#include "config/ticker_table.h"   // ticker_runtime_t, MAX_TICKERS

// Serialize the device's ticker rows into chunked cmd:"report" frames and emit them over `link` (issue
// #105), so a fresh hub can adopt the list the device already holds. Plans once, then for each chunk
// flush()es the prior chunk out BEFORE enqueuing the next: the whole report of a large list exceeds the
// link's bounded send buffer, and an all-or-nothing send() would reject the later parts -- so without the
// per-chunk flush a big list is never delivered and a fresh hub never adopts it (issue #106). The flush
// keeps no more than one chunk buffered at a time.
//
// Returns the number of chunks emitted (>= 0; 0 when there is nothing emittable), or -1 if a chunk failed
// to serialize or enqueue -- the caller does NOT latch its once-per-connection flag and retries the whole
// report (a retry restarts at part 0, which restarts the hub's accumulator, so a partial never half-adopts).
// A null link returns 0. Caller-owned: reading the runtime table into `rows` and the connection gating.
int hub_emit_report(HubLink* link, const ticker_runtime_t* rows, int count);
