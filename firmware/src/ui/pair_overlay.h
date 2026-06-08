#pragma once

// P2 BLE pairing overlay (D3). Polled from the LVGL loop (Core-1): shows a centered card with the
// 6-digit passkey while a central is bonding, hides it when pairing ends. No-op unless the hub link
// is actively pairing, so it costs nothing in BEACON_DEV / idle builds.
void pair_overlay_service(void);
