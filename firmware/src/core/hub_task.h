#pragma once
#include <stdbool.h>
#include <stdint.h>

// Hub-plane wiring (P2, Core-0). Owns the Bluedroid HubLink: pumps loop(), routes inbound status
// frames into ds_set_usage/ds_set_buddy, flips to ST_HUB_OFFLINE on disconnect, and tracks the
// running-min internal heap for the coexistence re-measure (tech.md §8). Started in the non-dev
// build alongside fetch_task; under BEACON_DEV the seed fakes the hub plane instead.
#ifdef __cplusplus
extern "C" {
#endif

void hub_task_start(void);

// True while a central (the macOS hub) is connected over BLE. Read from Core-1 (the UI) to drop the
// first-boot "pair with the hub" card once pairing actually succeeds.
bool hub_is_connected(void);

// --- hub-sourced weather (CONTRACT.md §A) ---
// The hub polls Open-Meteo on our behalf so the Wi-Fi radio can stay down. hub_task records the
// arriving reading's fetch timestamp here; fetch_task (also Core-0) reads it to decide whether to skip
// its own weather slot. Kept as a plain timestamp rather than a bool so the fallback is time-based:
// a hub that stops sending ages out on its own without needing a disconnect event.
// 0 => nothing received this boot. Both are cheap enough to call from either task.
void     hub_note_weather(uint32_t fetch_ts);
uint32_t hub_weather_ts(void);

// True when hub weather is fresh enough that the device should NOT spend a fetch (and a radio wakeup)
// on its own. `now` is wall-clock epoch seconds; a zero/unknown clock reports false.
bool     hub_weather_is_fresh(uint32_t now);

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

// Tap-to-open: ask the hub to focus the terminal/editor for session `id` (issue #110, P2-b). Builds
// the §7.1 "open" command frame and enqueues it via HubLink::send. Returns true if accepted for
// transport. With no hub link (BEACON_DEV) returns true so the UI path can still be exercised.
bool buddy_open(const char* id);

#ifdef __cplusplus
}
#endif
