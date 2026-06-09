#pragma once
#include <stdbool.h>

// First-boot WiFi provisioning (FR-SET-1): when no creds are stored, the device hosts a
// "Beacon-setup" SoftAP + captive portal; the user submits SSID/pass -> stored in NVS -> reboot to STA.
// No on-screen keyboard (tech.md §6). provision_loop() is pumped from the Arduino loop alongside LVGL.
#ifdef __cplusplus
extern "C" {
#endif

bool        provision_needed(void);   // true when NVS has no saved networks
void        provision_begin(void);    // boot path: bring up AP + DNS + HTTP; reboots after a successful save
void        provision_loop(void);     // pump DNS + HTTP (boot reboots after save; runtime mirrors net's AP radio)
bool        provision_active(void);
const char* provision_ap_ssid(void);  // the AP name shown to the user

// Runtime "add network" portal (FR-SET-1, on demand, no reboot). net owns the AP radio (Core-0); these
// run the captive DNS/HTTP servers on Core-1 from provision_loop once net brings the radio up. The UI
// requests the portal via net_request_provision() and polls provision_runtime_saved() for completion.
bool        provision_servers_up(void);          // net (Core-0) waits for false before dropping the AP radio
bool        provision_runtime_saved(void);       // a network was saved via the runtime portal
void        provision_runtime_clear_saved(void); // UI clears before (re)opening the portal

#ifdef __cplusplus
}
#endif
