#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "core/screen_state.h"   // data_err_t

// Device-direct plane (tech.md §6/§9). WiFi STA from NVS creds, event-driven (never blocks the LVGL
// loop). Starts NTP on GOT_IP. One cert-validated TLS socket at a time, serialized by a mutex.
#ifdef __cplusplus
extern "C" {
#endif

void net_begin(void);   // STA mode + WiFiMulti from the saved list; register events
bool net_is_up(void);   // associated + has IP

void net_service(void);          // Core-0 (fetch task): apply saved-list changes + gated WiFiMulti reconnect
void net_set_enabled(bool en);   // Connect/Disconnect toggle (pause/resume auto-join)
bool net_is_enabled(void);

// Runtime "add network" portal: the UI requests it here; net_service (Core-0) owns the AP radio so the
// UI never touches WiFi. provision_loop (Core-1) reads these to run the captive servers when the AP is up.
void net_request_provision(bool on);   // UI (Core-1): bring the setup AP up/down
bool net_provision_requested(void);    // provision (Core-1): the portal is wanted
bool net_provision_radio_up(void);     // provision (Core-1): net has the AP radio up; start the servers

// Short human status for the Settings Wi-Fi row: "<SSID> <ip>", "CONNECTING", "OFF", or "not set".
// Never includes the password. Writes a NUL-terminated string into buf.
void net_status_str(char* buf, size_t cap);
void net_active_ssid(char* out, size_t cap);   // connected SSID, or "" — for the panel's active marker

// Single HTTPS GET, cert-validated against ROOT_CA_BUNDLE. Writes up to cap-1 body bytes + NUL into
// out; sets *status to the HTTP code (0 on transport failure). Returns ERR_NONE on a 2xx with body,
// else an ERR_* (ERR_NO_ROUTE/ERR_TIMEOUT/ERR_RATE_LIMITED/ERR_HTTP). Serialized; safe from the
// Core-0 fetch task only (not the UI). hdr_n custom request headers (e.g. Yahoo User-Agent).
data_err_t net_https_get(const char* host, const char* path,
                         const char* const* hdr_keys, const char* const* hdr_vals, int hdr_n,
                         char* out, size_t cap, int* status);

// Fetch task (Core-0) only. Reuse the kept-alive HTTPS socket across same-host fetches (#61):
// net_open_host() reports the open connection's host ("" if none) so the scheduler can drain same-host
// due slots over one handshake; net_close_idle() drops the socket once a sweep goes idle.
const char* net_open_host(void);
void        net_close_idle(void);

#ifdef __cplusplus
}
#endif
