# Wi-Fi Management — Design

> Status: approved in brainstorming 2026-06-08; revised after code review (Codex + subagent). Branch
> context: `feat/p1-ambient-screens`. Supersedes the hidden long-press re-provision with a
> discoverable tap → Wi-Fi panel, and adds multi-network support. 2026-06-09: the panel gains a
> **+ add network** button that re-opens the setup portal on demand, with **no reboot**. Conventions:
> `docs/tech.md` §6/§9/§10.

## 1. Purpose

Replace the undiscoverable, unreliable **long-press to re-provision** with a **tap on the Settings
"Wi-Fi" row** that opens a Wi-Fi panel. Support **multiple saved networks** so the device auto-joins
whichever is in range as the user moves (home / office / hotspot) without re-provisioning each time.

## 2. Behavior model (decided)

- **Adding a network:** only via the `Beacon-setup` SoftAP portal on the phone (no on-screen keyboard).
  The portal **appends** to a saved list instead of overwriting. It opens on first boot (empty list), the
  touch-hold boot hatch, or **on demand from the panel's "+ add network"** button — the on-demand path
  re-opens the portal live and applies the new network **without rebooting** (only first-boot/hatch reboot).
- **Auto-connect:** the device uses `WiFiMulti` to join the **strongest available** saved network and
  re-evaluates if it drops. Joining is automatic — no manual "connect to this specific one."
- **On-device panel** (a single shared, themed overlay): current status · **list of saved networks**
  (active one marked) · global **Connect / Disconnect** toggle (pause/resume auto-join) · per-network
  **Forget** (tap row → confirm).
- **Entry:** tap the "Wi-Fi" row in Settings (any theme). **Exit:** an explicit **‹ Back** control (no
  swipe-to-dismiss).

## 3. Concurrency model (the crux — from review)

`WiFiMulti.run()` does a **blocking** scan (~2s) + connect (`delay`s + a connect-wait loop, up to
~5–8s total). It therefore has exactly one owner and one calling context:

- **Only `core/fetch_task` (Core-0) calls `wifimulti_run()`**, and **only when `!net_is_up()`** (a
  gated reconnect). Never every iteration while connected (would stall the staleness sweep), never from
  the UI (Core-1) — that would trip the loop WDT.
- **List-change handlers never call `run()`.** A portal *add* does a cheap `wifiMulti.addAP()` + sets a
  "dirty" flag. A *forget* rebuilds the AP set (WiFiMulti has no remove-AP) under the lock + sets the
  flag. The Core-0 loop consumes the flag and re-evaluates on its next pass.
- **`wifi_list` is shared across cores** (Core-1 panel/portal mutate; Core-0 reads to rebuild). All
  mutation + serialization is guarded by a **`ds_lock`-style mutex** (host-portable: `std::mutex`
  native / FreeRTOS device) owned by the persistence layer — **not** inside the pure `wifi_list` module
  (keeps it Arduino-free + host-testable).
- **Forget the active network does NOT tear down a live socket.** If Core-0 has an in-flight
  `net_https_get` (TLS mutex held) on the network being forgotten, it finishes; only the next
  `wifimulti_run()` re-evaluates. Forget updates the saved set, not the live connection.
- **Runtime re-provisioning also owns the radio on Core-0.** The panel's *+ add network* sets a request
  flag; `net_service` (Core-0) brings the `Beacon-setup` SoftAP up/down (`WIFI_AP_STA`, keeping the live
  STA link) and skips `run()` while it is up. The captive DNS/HTTP servers run on Core-1 (`provision_loop`).
  Four single-writer `volatile` flags coordinate the two cores: request (UI→net), radio-up (net→portal),
  servers-up (portal→net teardown guard), saved (portal→UI). On teardown the dirty flag drives the
  AP-set rebuild — the same path as add/forget — so the new net is applied once, no reboot.

## 4. Components

| Module | Responsibility |
|---|---|
| `core/wifi_list.{h,cpp}` | **Pure** (Arduino-free, host-tested) list logic over an in-memory `wifi_list_t`: append with SSID de-dup (re-adding an SSID updates its password, no growth), remove by SSID, count, **`WIFI_MAX_SAVED` = 6**, **reject** add when full (returns false; no silent eviction). No locking, no NVS here. |
| `core/nvs.{h,cpp}` | Gains `nvs_get_wifi_list` / `nvs_set_wifi_list` (one `putBytes`/`getBytes` blob) next to the existing keys (no separate `wifistore` file). Holds the **mutex** guarding the list. `nvs_migrate_wifi()`: if the blob key is **absent**, seed the list from the legacy `wssid`/`wpass` (if present), write the blob (even if empty, so the key now exists), and **delete the legacy `wssid`/`wpass` keys** — idempotent, runs once. |
| `core/net.{h,cpp}` | Replace single `WiFi.begin` with `WiFiMulti`: load saved list → `addAP` each. **`WiFi.setAutoReconnect(false)`** and **remove the `WiFi.reconnect()` call** from the disconnect event handler — WiFiMulti owns reconnection (the old auto-reconnect fights it and the toggle). `net_set_enabled(bool)`: Disconnect = `WiFi.disconnect()` + skip `run()`; Connect = clear the pause + let Core-0 `run()`. `wifimulti_run()` (Core-0 only). `net_wifi_changed()` flag for add/forget. `net_status_str` unchanged (already uses `WiFi.SSID()`). Keep `WiFi.persistent(false)`. Keep the `s_ntp_started` SNTP guard (do **not** reset on Disconnect, so Connect doesn't re-init SNTP). |
| `core/provision.cpp` | Portal **appends** via the list API (replaces the `nvs_set_wifi` overwrite at the save handler). If the list is full, the portal returns a "list full — forget one on the device" message instead of saving. Launched by first-boot (empty list), the touch-hold boot hatch, or **on demand from the panel**. The runtime path keeps the AP radio in `net` (Core-0) and runs the captive servers here on Core-1 (`provision_loop`); its save **appends without rebooting** (`s_runtime` branch — the boot save still reboots). Skips the ~2 s network scan at runtime (manual SSID entry) to keep the LVGL loop responsive. |
| `ui/wifi_panel.{h,cpp}` | **One shared** modal overlay on `lv_layer_top`, fully **self-contained** (references NO carousel page object — those are destroyed by `on_theme`/`lv_obj_clean`), styled from `theme_active()` tokens, built once. `wifi_panel_open()` + internal close. Renders status + scrollable saved list + Connect/Disconnect toggle + **+ add network** + ‹ Back; tapping a row opens an in-overlay **Forget "X"?** confirm; **+ add network** opens an in-overlay setup card that requests the runtime portal and polls `provision_runtime_saved()` for completion (closing it, or the panel, tears the AP down). (Theme can't change while open — the theme picker lives in Settings, which the panel covers — so stale styling isn't reachable.) |
| `ui/carousel.{h,cpp}` | New `carousel_set_swipe_enabled(bool)` — toggles `LV_OBJ_FLAG_SCROLLABLE`/scroll-dir on `s_pager`. The panel suspends swipe on open and **unconditionally restores it on close** (idempotent), so no code path can leave the carousel frozen. |
| Settings views (×7) | The "Wi-Fi" row changes from **long-press → re-provision** to **short tap (`LV_EVENT_CLICKED`) → `wifi_panel_open()`**. Removes the long-press cb. (Pattern verified on `settings_calm`; apply to all 7.) |
| `main.cpp` | Boot order adds `nvs_migrate_wifi()` early; touch-hold provisioning hatch unchanged. |

## 5. Data flow

```
boot:        nvs_migrate_wifi(); nvs_get_wifi_list() -> net: WiFiMulti.addAP(each)
                 -> Core-0 fetch_task: if !net_is_up() wifimulti_run() (blocking, gated) -> join best
portal add:  wifi_list append (dedup; reject if full) -> nvs_set_wifi_list -> wifiMulti.addAP + dirty flag
panel add:   net_request_provision(true) -> Core-0 brings up Beacon-setup (AP_STA) -> portal save appends
                 -> net_request_provision(false) -> Core-0 drops AP; dirty flag -> rebuild -> join (no reboot)
panel forget:lock; wifi_list remove(ssid); nvs_set_wifi_list; rebuild WiFiMulti set; dirty flag; unlock
                 (active socket, if any, finishes; next run() re-evaluates -> next best or offline)
toggle:      net_set_enabled(false) -> WiFi.disconnect(), pause run() (device-plane -> ST_OFFLINE)
             net_set_enabled(true)  -> resume; Core-0 run() reconnects
```

## 6. Edge cases / error handling

- **Forget active network:** saved set updated under lock; live fetch finishes; next gated `run()`
  joins the next reachable saved net or goes `ST_OFFLINE` (honest, as today).
- **Forget last / empty list:** offline now; next boot has empty list ⇒ `provision_needed()` ⇒ portal.
  Re-add live via the panel's **+ add network** (or the boot hatch portal) — no reboot needed.
- **List full (6) on portal add:** **reject** with a portal message ("forget one on the device first").
  No silent eviction — respects the user's explicit-forget model. De-dup first (re-adding a known SSID
  just updates its password).
- **Disconnect toggle:** WiFi truly paused (auto-reconnect already off); weather/finance/geoip go
  `ST_OFFLINE`; clock holds via RTC. SNTP guard untouched so Connect doesn't re-init.
- **Theme switch:** the panel survives on `lv_layer_top` (not cleaned by `on_theme`) but holds no
  page pointers; it can't be open during a theme change anyway (it covers the theme picker).
- **Restore-swipe guarantee:** carousel swipe is restored unconditionally in the panel close handler.

## 7. Out of scope (v1)

On-device password entry (portal only) · manual per-network connect (auto only) · live signal-strength
bars (needs a scan on open — v1 marks only the active network) · any per-theme bespoke panel (built once).

## 8. Testing

- **Host (native) unit tests** — `core/wifi_list` (added to `[env:native]` `build_src_filter`):
  table-driven — append, SSID de-dup updates password, remove by SSID, remove-active, **reject when
  full (cap 6)**, empty-list, count. Pure, Arduino-free (lock + NVS live outside this module).
- **On-device (human) acceptance:** tap Wi-Fi row opens the panel in every theme; **+ add network**
  opens the portal live, applies the new net **with no reboot**, and tears the AP down on close (of the
  card or the panel); add a 2nd network and confirm both auto-join while moving between them; forget
  (incl. the active one) behaves; Disconnect/Connect toggles and Connect re-joins; Back always restores
  swipe; boot hatch still forces the portal; reconnect roams to the strongest saved net.

## 9. Conventions / security

`tech.md` §10: no I/O on the LVGL loop (`WiFiMulti.run()` Core-0 only, gated on `!net_is_up()`), honest
`screen_state_t`, **never log or display passwords** (SSID-only in logs, incl. eviction/reject), ASCII
only, surgical diffs, no hardcoded theme colors/fonts in the panel (tokens only), table-driven tests.
Passwords live only in NVS.
