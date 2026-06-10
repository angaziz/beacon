# Runtime "Add network" button (no-reboot WiFi provisioning)

## Problem

The device remembers up to 6 WiFi networks (`WIFI_MAX_SAVED`) and auto-joins the
strongest in range via `WiFiMulti`. But the **only** way to register a 2nd/3rd
network today is an undiscoverable boot gesture: hold a finger on the screen
while powering on, which re-opens the `Beacon-setup` SoftAP + captive portal
(`main.cpp:89-95`). Users can't find it.

Add a visible **"+ add network"** button to the WiFi panel that re-opens the
setup portal on demand, **without rebooting**.

## Why no reboot

The existing portal reboots on save (`provision.cpp:72-75` -> `ESP.restart()`)
only because provisioning is a *separate boot mode* that never starts the normal
networking stack (`net_begin`, `WiFiMulti`, the reconnect loop). With nothing
running to apply the new creds, rebooting into the `else` branch is the simplest
correct thing.

A runtime button has no such problem: the app is **already** in normal mode
(`net_begin` ran, `net_service` is alive). Adding a network is also already a
live, no-reboot operation today — **Forget** uses the NVS dirty-flag path
(`nvs_wifi_forget` -> `net_service` rebuilds `WiFiMulti`). Add should be
symmetric. Rebooting to add but not to forget would be inconsistent.

## The one real constraint: WiFi is Core-0 only

The codebase's explicit invariant (net.cpp:17-18, 82): **the UI never touches the
`WiFi` singleton.** All radio (dis)connect/scan happens in `net_service()` on
Core-0 (fetch task, ~1s cadence). `provision_loop()` pumps DNS/HTTP on Core-1
(~5ms cadence).

Therefore the runtime portal splits ownership by core, coordinated with
single-writer flags (no locks needed):

- **Radio** (`WiFi.mode`, `softAP`, `softAPdisconnect`) -> **Core-0**, inside
  `net_service`. Honors the invariant; avoids racing `WiFiMulti.run()`.
- **DNS + HTTP servers** (the captive portal) -> **Core-1**, inside
  `provision_loop`, started once the radio is up.

### Flags (each written by exactly one core)

| Flag | Writer | Reader | Meaning |
|------|--------|--------|---------|
| `s_prov_req` (net) | UI / Core-1 | net / Core-0 | desired portal on/off |
| `s_prov_radio` (net) | net / Core-0 | provision / Core-1 | AP radio is up |
| `s_servers_up` (provision) | provision / Core-1 | net / Core-0 | DNS/HTTP running |
| `s_runtime_saved` (provision) | provision / Core-1 (handle_save) | UI / Core-1 | a net was saved |

### Lifecycle

```
TAP "+ add network"
  UI: provision_runtime_clear_saved(); net_request_provision(true); open overlay
  Core-0 net_service:  s_prov_req && !s_prov_radio
                       -> WiFi.mode(AP_STA); softAP("Beacon-setup"); s_prov_radio=true
  Core-1 provision_loop: radio up && want && !servers
                       -> start DNS + HTTP (no network scan; manual SSID entry); s_servers_up=true
  overlay: "join Beacon-setup, then enter your Wi-Fi"

USER submits SSID/pass on phone
  Core-1 handle_save: nvs_wifi_add(...) (sets NVS dirty); s_runtime_saved=true;
                      serve "Saved." (NO reboot)

UI tick sees provision_runtime_saved()
  -> show "added"; net_request_provision(false); clear saved

  Core-1 provision_loop: !want && servers -> stop DNS + HTTP; s_servers_up=false
  Core-0 net_service:    !s_prov_req && s_prov_radio && !servers_up
                       -> softAPdisconnect(true); WiFi.mode(STA);
                          clear dirty + rebuild_aps()  (WiFiMulti now knows the new net)
                       -> falls through to normal reconnect

CANCEL/CLOSE early: same teardown via net_request_provision(false).
```

Because Core-1 (5ms) runs far faster than Core-0 (1s), servers are always torn
down before Core-0 next runs; `s_servers_up` is the guard for the rare interleave.

### Why current connection is undisturbed

`WIFI_AP_STA` keeps the STA link up. `net_service` early-returns while the radio
is provisioned, so it issues no `run()`/`disconnect()`. Adding a *backup* network
while connected leaves you connected; `WiFiMulti.run()` only re-evaluates when
`!net_is_up()`. On teardown, `rebuild_aps()` keeps the active net if still saved.

## Changes

| File | Change |
|------|--------|
| `core/provision.h` | declare runtime server lifecycle + saved-flag accessors |
| `core/provision.cpp` | runtime portal: no-scan server start/stop, `s_runtime` branch in `handle_save` (no reboot) + `provision_loop` |
| `core/net.h` | declare `net_request_provision`, `net_provision_requested`, `net_provision_radio_up` |
| `core/net.cpp` | AP radio up/down state machine at the top of `net_service`; include `provision.h` |
| `ui/wifi_panel.cpp` | "+ add network" button + add overlay driven by the panel's 1s tick |

## Non-goals

- No on-device keyboard; SSID/pass entry stays on the phone via the portal
  (tech.md §6). Runtime portal skips the ~2s network scan to keep the UI
  responsive — the user types the SSID (the portal's manual field).
- Boot/force-provision path (with reboot) is unchanged.

## Verification

PlatformIO is not available in this environment, so this can't be compiled here.
Build + hardware check required before merge:

1. `pio run` (firmware) compiles clean.
2. Connected to net A, tap "+ add network" -> `Beacon-setup` appears; phone
   captive page opens; submit net B -> "added"; portal closes; **no reboot**;
   list shows A + B; still connected to A.
3. Disconnected (A out of range), add B -> after close, device joins B.
4. Tap "+ add network" then CLOSE without saving -> portal gone, normal WiFi
   resumes, no leftover AP.
5. List full (6 saved) -> portal shows "Saved networks full".
