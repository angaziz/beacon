# Implementation Plan — Issue #11: Distinguish BLE connection states + icon/state glyphs

## Scope decision
Six target states, all reachable, all kept. Rare `.resetting`/`.unsupported`/`.unknown` fold into one generic `.unavailable` (no distinct user remediation). No speculative states.

| State | Reachable via |
|---|---|
| Bluetooth off | `CBManagerState.poweredOff` |
| Permission needed | `.unauthorized` |
| Searching for device… | `.poweredOn` + scanning, no peripheral yet |
| Connecting… | peripheral discovered, not yet `isConnected` |
| Connected | `isConnected == true` |
| Disconnected — reconnecting | drop after a prior successful connect |

## Part A — `BeaconCentral.swift`: emit explicit link phase

A1. Add enum before class (line 9):
```swift
enum LinkPhase: Equatable {
    case bluetoothOff
    case unauthorized
    case unavailable
    case searching
    case connecting(String)
    case connected(String)
    case reconnecting
}
```

A2. Replace callback (line 22): `var onStatusChange: ((Bool, String?) -> Void)?` => `var onPhaseChange: ((LinkPhase) -> Void)?`. Keep `isConnected`/`connectedName` (used by frame-gating).

A3. Private state near line 27:
```swift
private var phase: LinkPhase? = nil   // nil sentinel: forces first emit even if first real state folds to .unavailable
private var hadConnection = false
private func setPhase(_ p: LinkPhase) {
    guard p != phase else { return }
    phase = p
    onPhaseChange?(p)
}
```
(Codex fix: `nil` start guarantees the first phase always emits, so an initial `.unavailable`/`.unsupported` reaches AppDelegate instead of being suppressed against the default.)

A4. Drive phase from lifecycle:
- `centralManagerDidUpdateState` (101–106): replace `default:` with explicit `.poweredOn`=>beginScan, `.poweredOff`=>`setPhase(.bluetoothOff)`, `.unauthorized`=>`setPhase(.unauthorized)`, `default`=>`setPhase(.unavailable)` (each also `isConnected=false`).
- `beginScan` (63–68): trailing emit => `setPhase(hadConnection ? .reconnecting : .searching)`.
- `didDiscover` (108–117): after `central.connect`, `setPhase(.connecting(name))`.
- `isConnected` didSet (24–26): when true => `hadConnection=true; setPhase(.connected(connectedName ?? ""))`. When false, do not emit (disconnect path owns next phase).

A5. Initial `phase=nil`; `centralManagerDidUpdateState` drives first real phase, which always emits (nil != any case). No extra emit.

## Part B — `AppDelegate.swift`: forward phase

B1. `startCentral` (60–73): replace `onStatusChange` closure with `onPhaseChange = { [weak self] phase in Task { @MainActor in self?.refreshLink(phase) } }`.

B2. `refreshLink` (76–84): map `LinkPhase` => `MenubarController.Link`, preserve side effects (`setAlert(nil)` on connect, `bridge?.setDeviceConnected(connected)`).

## Part C — `MenubarController.swift`

C1. Expand `Link` enum (line 8): `bluetoothOff, unauthorized, unavailable, searching, connecting(String), connected(String), reconnecting`. Remove dead `.scanning`/`.disconnected`. Initial default => `.searching`.

C2. Status text + remediation (77–81): full case set. Add `fixLine` NSMenuItem (after statusLine in buildMenu, hidden by default). Two distinct fixes (Codex fix — different panes):
- `.bluetoothOff`: title "Open Bluetooth settings…", action opens `x-apple.systempreferences:com.apple.Bluetooth` (radio pane).
- `.unauthorized`: title "Open Privacy settings…", action opens `x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth` (app-permission pane).
For both: `action=#selector(openLink)`, `target=self`, `isEnabled=true`, unhidden; hidden+disabled otherwise. Store the target URL on the controller so a single `@objc openLink` selector opens whichever fix URL the current state set. Guard each `NSWorkspace.shared.open` with fallback to System Settings root if the pane URL fails.

**NSObject requirement (Codex fix):** `MenubarController` is currently `final class MenubarController` (pure Swift). Target-action requires an Obj-C-dispatchable selector — change to `final class MenubarController: NSObject` and mark the action `@objc private func openLink()`. Keep `@MainActor`.

C3. Bar icon (init line 30, render line 82, replace `barTitle`): SF Symbol image via `NSImage(systemSymbolName:accessibilityDescription:)`. Connected/neutral => template (native tint). Working states (searching/connecting/reconnecting) => dimmed `.secondaryLabelColor`. Fault states => `exclamationmark.triangle.fill` orange/red. Alert override keeps red triangle. Set `accessibilityDescription` per state for VoiceOver.
**contentTintColor must be set OR cleared on every render (Codex fix):** AppKit only tints template images, and a stale `contentTintColor` leaks from a fault/working state back into neutral/connected. Always assign `button.contentTintColor = <color or nil>` and `image.isTemplate = true` in every `applyBarIcon()` call; never leave it unset.

C4. Pairing hint (pairLine, lines 25/43–44): in render, show only for `.searching`/`.connecting`, hide otherwise.

C5. Live sync age (84–89): use absolute HH:MM via cached `DateFormatter` (`.short` time). "Last sync: never" for nil. Avoids stale-read with no timer.

## Sequencing
1. C1 (enum) → compiler flags consumers.
2. A1–A5 (BeaconCentral).
3. B1–B2 (AppDelegate).
4. C2–C5 (MenubarController).
5. Build via `./build-app.sh`; verify states from signed `.app` bundle.

## Risks
- Template vs colored icon: template for neutral/connected, colored `contentTintColor` for fault/working.
- `x-apple.systempreferences:` pane id can drift; guard with fallback.
- `onStatusChange` referenced only at BeaconCentral.swift:22 + AppDelegate.swift:61; safe rename.
- Pre-existing cross-queue read of `central.isConnected` at AppDelegate.swift:146 (frame-gating on main while mutated on the central queue) is NOT introduced or worsened by this change — out of scope for #11. Left as-is per surgical-change rule.
