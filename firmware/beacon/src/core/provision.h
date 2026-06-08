#pragma once
#include <stdbool.h>

// First-boot WiFi provisioning (FR-SET-1): when no creds are stored, the device hosts a
// "Beacon-setup" SoftAP + captive portal; the user submits SSID/pass -> stored in NVS -> reboot to STA.
// No on-screen keyboard (tech.md §6). provision_loop() is pumped from the Arduino loop alongside LVGL.
#ifdef __cplusplus
extern "C" {
#endif

bool        provision_needed(void);   // true when NVS has no saved networks
void        provision_begin(void);    // bring up the AP + DNS + HTTP portal
void        provision_loop(void);     // pump DNS + HTTP; reboots after a successful save (no-op if inactive)
bool        provision_active(void);
const char* provision_ap_ssid(void);  // the AP name shown to the user

#ifdef __cplusplus
}
#endif
