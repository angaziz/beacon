#pragma once

// Device-direct data scheduler (P1). A single Core-0 task that: sweeps staleness ~1/s, and when the
// link is up and time is known, fetches the most-overdue due source (weather + each ticker) one at a
// time through the shared TLS socket. Replaces the dev_seed stale_task in non-dev builds.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void fetch_task_start(void);

// One shared 8KB fetch scratch (#65 M6). All fetches run on this single Core-0 task and serialize
// behind the TLS mutex, so they never need concurrent buffers; this replaces three per-module static
// buffers (8192 + 4096 + 1024 = 13.3KB of internal .bss) with one. Use only from the fetch task.
char*  fetch_scratch(void);
size_t fetch_scratch_cap(void);
#ifdef __cplusplus
}
#endif
