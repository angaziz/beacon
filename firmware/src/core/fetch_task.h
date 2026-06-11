#pragma once

// Device-direct data scheduler (P1). A single Core-0 task that: sweeps staleness ~1/s, and when the
// link is up and time is known, fetches the most-overdue due source (weather + each ticker) one at a
// time through the shared TLS socket. Replaces the dev_seed stale_task in non-dev builds.
#ifdef __cplusplus
extern "C" {
#endif
void fetch_task_start(void);
#ifdef __cplusplus
}
#endif
