# Hub BLE Link Toggle - Design

Date: 2026-08-02
Component: hub
Status: approved (brainstormed 2026-08-02)

## Problem

The hub starts scanning for a Beacon device at launch and never stops. For a user who has not paired a device yet (or does not intend to right now), `BeaconCentral.beginScan()` runs indefinitely. Active scanning is CoreBluetooth's highest-power mode, and the menubar permanently shows "Searching" with no way to opt out.

Note: once a device has paired, disconnects already use a low-power pending `connect()` (issue #63), which costs essentially nothing. The toggle exists primarily for the never-paired state, but per the decision below it disables the whole link.

## Decisions

| Question | Decision |
|---|---|
| Scope | Kill the whole BLE link: toggle off = disconnect if connected, no pending connect, no scan |
| Persistence | Persisted in UserDefaults, default ON (existing users see no change) |
| UI placement | One switch row in the menubar popover, next to the link status |
| Paired identity | Kept across toggle off/on; re-enabling reconnects without re-pairing |
| Menubar icon change while off | Out of scope; the popover row and status text are the only indicators |

## Behavior

- New persisted setting `bleEnabled` (UserDefaults, default `true`). The key may be absent on first run; absent MUST resolve to `true` (use `object(forKey:) == nil` sentinel, not `bool(forKey:)` which defaults to `false`).
- Toggle OFF: `stopScan()`, cancel any live connection, drop the pending connect, emit a new `LinkPhase.disabled`. The device sees a normal disconnect and shows its hub-offline state.
- Toggle ON: re-derive from `central.state`. If `.poweredOn`, run the existing `reconnect()` path; otherwise emit the matching transport phase (`.bluetoothOff` / `.unauthorized` / `.unavailable`) so the user is not stuck on a stale "off" label with Bluetooth actually unavailable.
- Pending-connect on re-enable applies within one process lifetime only: `knownId`/`knownName` are memory-only, so after an app relaunch the ON path scans (same as today's launch behavior). No identity persistence is added.
- At launch with `bleEnabled == false`: `central.start()` still runs (so CBCentralManager reports Bluetooth state and permissions), but no scan or connect is issued.
- A Bluetooth off/on cycle, app relaunch, or `.poweredOn` state callback while disabled must NOT restart scanning.

## Changes

### BeaconCentral (`hub/Sources/beacon-hub/BeaconCentral.swift`)

1. Add `case disabled` to `LinkPhase`.
2. Add a queue-confined `enabled: Bool` flag (seeded via `start(enabled:)`).
3. `stop()`: set `enabled = false`, call `central.stopScan()` explicitly (the `handleDisconnect()` teardown never stops a scan; the only existing `stopScan()` lives in `didDiscover`), cancel any live/in-flight connection, nil peripheral/rx/tx, clear inbound, then emit `.disabled`. Do NOT touch the escalation failure count and do NOT call `reconnect()`. Keep `knownId`/`knownName`.
4. `resume()`: set `enabled = true`, reset the pairing escalation budget, then re-derive: `.poweredOn` => `reconnect()`; any other `CBManagerState` => emit the matching transport phase (a bare `reconnect()` call would no-op when Bluetooth is off and leave the phase stuck at `.disabled`).
5. Master-flag rule: while `enabled == false`, `.disabled` is the only phase ever emitted. This means guarding not just `beginScan()` / `reconnect()` / the `.poweredOn` branch, but ALL `centralManagerDidUpdateState` branches (`.poweredOff` etc. must not overwrite `.disabled`; the raw `CBManagerState` keeps being tracked so `resume()` reconciles correctly).
6. Guard delegate callbacks against in-flight races: `didDiscover` early-returns when `!enabled` (a discovery delivered right after `stopScan()` must not start a connect), and `handleDisconnect()` early-returns after teardown when `!enabled` (so the cancel issued by `stop()` echoing back via `didFailToConnect`/`didDisconnectPeripheral` neither burns the pairing-escalation budget nor emits `.pairingFailed`/reconnects).

### AppDelegate / UI (`hub/Sources/beacon-hub/`)

The UI does not consume `LinkPhase` directly; the full plumbing path is:

- `AppDelegate`: read `bleEnabled` at launch (absent key => `true`); pass to `central.start(enabled:)`. `refreshLink(_:)` maps `LinkPhase.disabled` to a new `MenubarController.Link` case.
- `MenubarController.Link`: add a `.disabled` case; `applyBarIcon()`'s exhaustive switch renders it with the existing non-connected icon (per the Decisions table: no distinct icon).
- `HubPanel.statusText`: new case => "Beacon link off".
- Toggle plumbing mirrors the mute toggle end to end: visual row in `DeckUI.ToggleRow`, binding in `HubPanel.TogglesModule`, intent forwarding in `MenubarController.wireModel()`, seeded in `HubViewModel.init`. Seeding MUST use the absent-key-means-true rule above (mute's literal `bool(forKey:)` pattern would silently default the link to off).
- Flipping the toggle persists the setting and calls `central.stop()` / `central.resume()`.
- "Forget device": disable the button in `SettingsPanel` while the link is off (single enforcement point; `forgetAndRescan()` would otherwise clear `knownId` and start a scan). "Try again" is unreachable while disabled (`.pairingFailed` is never emitted when `!enabled`).

### First-run setup interaction

`refreshLink` treats every non-connected phase as pairing `.bad`, and `SettingsWindowController.showIfNeeded()` auto-opens at launch until setup completes. Rule: when the persisted preference is off at launch, the setup window's auto-open is suppressed (do not nag a user who has intentionally turned the link off). The pairing check stays `.bad`, which already renders as a neutral open circle (not an error glyph), so no new `CheckState` case is needed. Setup completion still requires an actual successful pairing, so it simply stays incomplete until the user re-enables and pairs.

### Downstream consumers: no changes

Every consumer already guards on `isConnected` or the phase stream:

- Frame sends skip when not connected (`AppDelegate.swift` ticker/status paths).
- `UsagePoller` already throttles via `setDeviceConnected(false)`.
- Buddy permission prompts: when no device is connected, providers pass prompts through with no verdict (local approval UI decides; `ClaudeCodeProvider.permissionCore` / `HookBuddyProvider.permissionCore`), and `HookBuddyProvider.setDeviceConnected(false)` releases held prompts as pass-through. Disabled behaves exactly like today's disconnected state; no change to that policy.

## Error handling

- `.disabled` wins over transport states in the UI: while off, no other phase is emitted (enforced in `BeaconCentral` by the master-flag rule above). The raw `CBManagerState` is still tracked, so `resume()` lands in the correct phase.
- Toggling rapidly is safe: `stop()`/`resume()` are queue-serialized like all other link mutations.

## Testing

- `BeaconCentral` is CoreBluetooth-coupled and not host-testable; enable/disable decision logic that can live as a pure function goes in `BeaconHubKit` with table-driven tests (pattern: `UsagePollDecision`).
- Manual verification:
  1. Toggle off while connected: device shows hub offline; popover shows "Beacon link off".
  2. Relaunch hub: stays off, no scan (verify via Console or PacketLogger if in doubt).
  3. Toggle on: reconnects to the paired device without a pairing dialog.
  4. Toggle off, cycle macOS Bluetooth off/on: no scan starts.
