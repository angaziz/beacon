#pragma once
#include <stdbool.h>

// Hub-plane wiring (P2, Core-0). Owns the Bluedroid HubLink: pumps loop(), routes inbound status
// frames into ds_set_usage/ds_set_buddy, flips to ST_HUB_OFFLINE on disconnect, and tracks the
// running-min internal heap for the coexistence re-measure (tech.md §8). Started in the non-dev
// build alongside fetch_task; under BEACON_DEV the seed fakes the hub plane instead.
#ifdef __cplusplus
extern "C" {
#endif

void hub_task_start(void);

// Device->hub command helpers, safe to call from Core-1 (the buddy decide path). Build the §7.1
// command frame and enqueue it via HubLink::send (which copies + is thread-safe). Return true if
// accepted for transport. With no hub link initialized (BEACON_DEV), permission returns true so the
// on-device UI still clears locally for testing; launch returns false (nothing to launch into).
bool hub_send_permission(const char* id, bool approve);
bool hub_send_launch(const char* text);

#ifdef __cplusplus
}
#endif
