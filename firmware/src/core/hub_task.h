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

// Device->hub permission decision, safe to call from Core-1 (the buddy decide path). Builds the §7.1
// command frame and enqueues it via HubLink::send (which copies + is thread-safe). Returns true if
// accepted for transport. With no hub link initialized (BEACON_DEV), returns true so the on-device UI
// still clears locally for testing.
bool hub_send_permission(const char* id, bool approve);

// Centralized buddy decide path (issue #8): the single place a view calls to approve/deny the active
// prompt. Applies the canonical guard (present && not hub-offline/reconnecting && not already decided),
// enqueues the §7.1 command, and on success marks the prompt PROMPT_PENDING WITHOUT clearing present --
// the prompt is cleared only later by a truthful hub ack (hub_apply_ack). Returns true if enqueued.
bool buddy_decide(bool approve);

// Dismiss a prompt the hub said did not apply (PROMPT_TOO_LATE): clears present locally so the warning
// goes away. No-op for any other state. Returns true if a prompt was dismissed.
bool buddy_dismiss(void);

#ifdef __cplusplus
}
#endif
