# Issue #16 -- Lifecycle: login item + graceful quit drain + forget-device/re-pair

Branch: `fix/16-lifecycle`
Source audit: `docs/research/2026-06-08-hub-ux-audit.md` 3.6 (and 4 Tier-2 items 11/12/13).

Three INDEPENDENT, cleanly-separable parts. Reuse existing plumbing (the deny-with-message
path, the target/action + closure-callback menu pattern). No speculative flexibility. ASCII only.

Out of scope (do NOT touch): Keychain "Always Allow" persistence across rebuilds (cdhash issue, deferred).

---

## Grounding (verified by reading the code)

- Deny-with-reason already exists end-to-end. `ClaudeCodeBridge.respondDecision(_:allow:event:message:)`
  (line 487) -> `HookResponse.permission(event:allow:message:)` (Protocol.swift:96) emits the correct
  per-event shape with `message` as the deny cause. `finish(id:approve:capped:)` (line 297) is the single
  resolution point: it is idempotent (guarded by `Pending.done`), cancels the cap timer, clears
  waiting/prompt, logs, and calls `p.respond(approve)`. The offline/busy auto-deny paths already call
  `respondDecision(..., message: "...")` directly. The drain must reuse THIS, not duplicate it.

- `Pending.respond` closure (line 50) currently always calls `respondDecision(conn, allow:, event:)`
  with NO message. So `finish(id:, approve:false)` denies but with the generic "Denied on Beacon device"
  string. To get a deny-WITH-REASON on drain we need the message to flow through. Two options (decision below).

- The HTTP write is async: `respond(...)` (line 498) calls `conn.send(content:completion: .contentProcessed { conn.cancel() })` on the bridge `queue`. So after `finish()` returns, the bytes are NOT yet guaranteed flushed to the socket. A drain that returns `.terminateNow` immediately can race the process exit ahead of the socket write => CC sees a dropped connection (fail-OPEN per CONTRACT.md C.3) instead of our deny. Hence `.terminateLater` + a short flush wait is needed.

- AppDelegate (`@MainActor`) owns `bridge: ClaudeCodeBridge?`. There is NO `applicationShouldTerminate`/
  `applicationWillTerminate` today. The bridge runs all state on its private serial `queue`.

- BeaconCentral: scanning is by NAME PREFIX "Beacon" (line 27), connect-on-discover (didDiscover line 135),
  pairing success = `didUpdateNotificationStateFor` notifying (line 182). `handleDisconnect()` (line 95)
  already does the full app-side reset: clears `connectedName`, `isConnected=false`, nils rx/tx, calls
  `central.cancelPeripheralConnection(peripheral)`, nils `peripheral`, clears `inbound`, and `beginScan()`.
  **There is NO persisted device identifier and NO `retrievePeripherals`/UserDefaults device id** -- the hub
  holds no durable bond reference of its own. This is the crux of the forget-device design (see Part 3).

- CONTRACT.md C.3: hub MUST return a 2xx deny within the hook window; non-2xx/timeout = fail-OPEN (CC proceeds).

- build-app.sh ad-hoc signs (`codesign --force --sign -`). Info.plist sets `LSUIElement` + bundle id
  `com.beacon.hub`. No ServiceManagement/SMAppService usage anywhere yet.

---

## Part 1 -- Login item (SMAppService.mainApp + menu toggle)

Goal: hub auto-starts after reboot via a toggleable login item; menu reflects current registration state.

### Steps

1. **New file `hub/Sources/beacon-hub/LoginItem.swift`** -- a tiny `enum LoginItem` wrapper over
   `SMAppService.mainApp` (import `ServiceManagement`). Keep it pure/UI-free so it is unit-testable in
   spirit and swappable. API:
   - `static var isEnabled: Bool` -> `SMAppService.mainApp.status == .enabled`.
   - `static func setEnabled(_ on: Bool) throws` -> `try register()` / `try unregister()`.
   - `static var status: SMAppService.Status` (exposed so the menu can show requires-approval too).
   WHY a wrapper: `SMAppService` is only meaningful from a real `.app` bundle (LaunchServices identity);
   isolating it keeps the call sites trivial and documents the ad-hoc-signing caveat in ONE place.

2. **`MenubarController.swift`** -- add a login-item toggle following the EXACT `muteLine` pattern, and make the
   controller the menu's delegate so status refreshes on OPEN (CODEX MUST-FIX #4: for an `LSUIElement` app,
   `applicationDidBecomeActive` is an unreliable refresh trigger; `menuWillOpen` is the right hook):
   - Add `private let loginItemLine = NSMenuItem(title: "Start at login", action: nil, keyEquivalent: "")`.
   - In `buildMenu()`: wire `loginItemLine` target/action `#selector(toggleLoginItem)`, add near the mute item.
   - Set `menu.delegate = self`; conform to `NSMenuDelegate` and implement
     `func menuWillOpen(_ menu: NSMenu) { onMenuWillOpen?() }`. Add `var onMenuWillOpen: (() -> Void)?`.
   - Add `var onToggleLoginItem: ((Bool) -> Void)?` (closure-callback, `onOpenSetup` style).
   - Add `func setLoginItemState(_ status: LoginItemStatus)` where `LoginItemStatus { enabled, disabled, requiresApproval }`
     (a tiny UI-facing enum mapped by AppDelegate from `SMAppService.Status`, so MenubarController does not import
     ServiceManagement). Render: `.enabled` => checkmark on, title "Start at login"; `.requiresApproval` =>
     checkmark off, title "Start at login (approve in Login Items)"; `.disabled` => checkmark off, plain title.
     This surfaces requires-approval honestly instead of a silent false "off" (CODEX MUST-FIX #3).
   - `@objc private func toggleLoginItem()` computes desired (`loginItemLine.state != .on`) and calls
     `onToggleLoginItem?(desired)`. It does NOT flip its own checkmark optimistically; AppDelegate calls back
     `setLoginItemState` with the RE-READ truth.

3. **`AppDelegate.swift`** -- wire it in a new `startLoginItem()` called from `applicationDidFinishLaunching`:
   - `menubar.onToggleLoginItem = { [weak self] on in self?.applyLoginItem(on) }`
   - `menubar.onMenuWillOpen = { [weak self] in self?.refreshLoginItem() }` (re-read on every menu open).
   - `refreshLoginItem()` maps `LoginItem.status` (`SMAppService.Status`) => `LoginItemStatus` and calls
     `menubar.setLoginItemState(...)`; call it once at launch too.
   - `applyLoginItem(_:)`: `do { try LoginItem.setEnabled(on) } catch { menubar.setAlert("Login item: \(error.localizedDescription)") }`
     then ALWAYS `refreshLoginItem()` (re-read truth). If the mapped status is `.requiresApproval`, also
     `menubar.setAlert("Approve Beacon in System Settings > General > Login Items")` (and optionally a
     `SettingsLinks` jump) so the needs-approval case is actionable, not silent (CODEX MUST-FIX #3).
   - Keep the `applicationDidBecomeActive` re-sync too (cheap, harmless), but the menu-open refresh is the
     reliable path for this accessory app.

### Verification (Part 1)
- `swift build -c debug` compiles (ServiceManagement links).
- `./build-app.sh run` -> open the menu: "Start at login" present, checkmark reflects current state.
- Toggle on -> confirm via `System Settings > General > Login Items` (or
  `sfltool dumpbtm | grep -i beacon`) that `com.beacon.hub` appears. Toggle off -> it disappears.
- Reboot (or log out/in) with it ON -> hub relaunches. Menu still shows checkmark on.
- Flip it off in System Settings while running, refocus the menu (triggers `applicationDidBecomeActive`)
  -> checkmark clears.

### Risk (Part 1) -- ad-hoc signing
`SMAppService.register()` under an ad-hoc signature (`codesign --sign -`) is the known soft spot. Realistic
behavior: registration generally SUCCEEDS for `SMAppService.mainApp` even ad-hoc, but macOS may surface a
"allow in Background/Login Items?" approval and the status can be `.requiresApproval`; the persisted record is
keyed to the bundle, and because the ad-hoc cdhash changes every `build-app.sh` rebuild, a previously-approved
item can show as a STALE/duplicate entry after a rebuild (same class of cdhash problem as the deferred Keychain
issue). Mitigation that stays in scope: surface `.requiresApproval` honestly (the menu reflects `LoginItem.isEnabled`
== false until approved; optionally word the failure alert to point at System Settings > Login Items) and document
that a clean Developer-ID/notarized build is what makes this fully stable. Do NOT attempt to auto-clean stale BTM
records (that is the deferred cdhash track). Flag to the user.

---

## Part 2 -- Graceful quit drain (deny-with-reason before terminate)

Goal: on quit, every still-HELD permission prompt is resolved as deny-with-reason and the deny actually
reaches CC over the socket BEFORE the process exits -- no dropped responder / hung CC prompt.

### Design decisions

- **Where:** implement `applicationShouldTerminate(_:)` returning `.terminateLater`, do the drain, then
  `NSApp.reply(toApplicationShouldTerminate: true)` once the deny writes have ACTUALLY flushed (or a bounded
  deadline elapses). WHY not `applicationWillTerminate`: by the time `willTerminate` fires the runloop is
  tearing down and the async `conn.send` completion may not flush -- we need the explicit "hold termination"
  handshake.

- **CODEX MUST-FIX #1/#2 -- completion-aware, not a fixed sleep.** A fixed `0.3s` wait is best-effort, not an
  ordering guarantee (and is exactly the kind of magic-number hack to avoid). Instead make the drain
  COMPLETION-AWARE: each deny's socket write signals a `DispatchGroup`; the caller replies to termination from
  `group.notify` OR a bounded 1.0s safety deadline (whichever first), and replies IMMEDIATELY when nothing was
  held. This guarantees the deny bytes are handed to the OS before exit in the common case, and never hangs Quit.

- **CODEX MUST-FIX #2 -- guard against a prompt arriving DURING the drain window.** Add a `terminating` flag on
  the bridge (set at drain start, queue-confined). `handlePermission` checks it FIRST and immediately denies any
  new prompt with the quit reason (no new held entry), so a prompt landing inside the termination window can't be
  dropped. Minimal: one Bool + one early-return branch.

- **What gets denied:** every entry in `bridge.pending` whose `done == false` (the genuinely-held prompts;
  resolved-but-retained entries used for the late-ack race must be skipped via the existing `!done` guard,
  which `finish()` already enforces). In practice this is at most ONE (single active prompt by design), but
  iterate the map so the contract is "drain ALL held" and it stays correct if that invariant ever loosens.

- **The deny reason:** reuse the deny-with-message path. The cleanest reuse: have the drain run `finish()`
  for each held id with a quit-specific message. Since `Pending.respond` is the closure that calls
  `respondDecision`, add the message at the source.

### Steps

1. **`ClaudeCodeBridge.swift` -- thread the deny message AND a send-completion through the deny path.**
   - Change `Pending.respond` from `(Bool) -> Void` to `(Bool, String?, (() -> Void)?) -> Void`
     (allow, optional deny message, optional onSent). At the creation site (line 283):
     `{ [weak self] allow, msg, onSent in self?.respondDecision(conn, allow: allow, event: event, message: msg, onSent: onSent) }`.
   - Add a defaulted `onSent: (() -> Void)? = nil` param to `respondDecision(...)` and to the low-level
     `respond(_:status:json:)`; invoke `onSent?()` INSIDE the existing `.contentProcessed { _ in conn.cancel() }`
     completion (line 505), after `conn.cancel()`. This is the seam that makes the drain completion-aware.
   - Give `finish` two new defaulted params: `finish(id:approve:capped:message: String? = nil, onSent: (() -> Void)? = nil)`;
     pass them into `p.respond(approve, message, onSent)`.
   - All EXISTING `finish`/`respond` call sites omit `message`/`onSent` => nil => unchanged behavior (the
     `.contentProcessed` still just cancels the conn). SURGICAL: one closure signature, defaulted params only.

2. **`ClaudeCodeBridge.swift` -- add `terminating` flag + the completion-aware drain entry point.**
   - Add `private var terminating = false` (queue-confined). In `handlePermission`, as the FIRST check
     (before offline/busy), `if terminating { respondDecision(conn, allow:false, event:event, message: <quit reason>); return }`
     so a prompt arriving during the drain window is denied immediately, never held (CODEX MUST-FIX #2).
   - Drain:
   ```
   // Quit drain (issue #16): deny every still-held prompt with a reason, and fire `completion` only once the
   // deny bytes have flushed to the socket (DispatchGroup over the per-send onSent), so CC gets a clean answer
   // instead of a dropped responder. Calls completion immediately when nothing is held. Queue-confined.
   func drainHeldPrompts(reason: String, completion: @escaping () -> Void) {
       queue.async { [weak self] in
           guard let self else { DispatchQueue.main.async(execute: completion); return }
           self.terminating = true
           let heldIds = self.pending.filter { !$0.value.done }.map(\.key)
           guard !heldIds.isEmpty else { DispatchQueue.main.async(execute: completion); return }
           let group = DispatchGroup()
           for id in heldIds {
               group.enter()
               self.finish(id: id, approve: false, capped: false, message: reason, onSent: { group.leave() })
           }
           group.notify(queue: .main, execute: completion)
       }
   }
   ```
   WHY `queue.async` (not `sync`): the caller is on the main thread and we hand back control via `completion`;
   no need to block main. Log line per drained id is already emitted by `finish()` (decision=deny).

3. **`AppDelegate.swift` -- hook termination (completion-aware, bounded).**
   ```
   func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
       guard let bridge else { return .terminateNow }
       var replied = false
       let reply = { if !replied { replied = true; NSApp.reply(toApplicationShouldTerminate: true) } }
       bridge.drainHeldPrompts(reason: "Beacon hub is quitting", completion: reply)
       // Safety cap so Quit never hangs if a socket write stalls; the drain replies earlier on real flush,
       // and immediately when nothing was held. A dropped conn would fail-OPEN per CONTRACT.md C.3.
       DispatchQueue.main.asyncAfter(deadline: .now() + 1.0, execute: reply)
       return .terminateLater
   }
   ```
   `reply` is idempotent (the `replied` latch) so whichever fires first -- real flush or the 1.0 s cap -- wins;
   the other is a no-op. Both run on the main queue, so no races on `replied`. `reply(toApplicationShouldTerminate:)`
   must be called on the main thread (satisfied).

### Verification (Part 2)
- Unit-testable seam: `drainHeldPrompts` + the `finish(message:)` path are queue-confined and message-carrying.
  Add a table-driven test in `BeaconHubKitTests` ONLY for the message shaping that lives in BeaconHubKit:
  `HookResponse.permission(event:allow:message:)` already shapes `decision.message` / `permissionDecisionReason`
  -- assert a quit reason renders into both PermissionRequest and PreToolUse shapes (cases: allow (no message),
  deny+message, deny+nil-message=generic). (The bridge itself is in the executable target and not unit-tested
  today; keep parity -- do not add a test target for it in this issue.)
- Manual: start a CC session, trigger a permission prompt so the device holds it (BuddyState shows the prompt),
  then Quit the hub. Expected: CC's TUI shows the tool DENIED with reason "Beacon hub is quitting" (not a hang,
  not fail-open). Confirm the hub log shows `perm id=... decision=deny`.
- Manual negative: Quit with NO prompt held -> drain calls `completion` immediately => instant quit (no 1 s wait).

---

## Part 3 -- Forget device / re-pair (no OS-level Bluetooth surgery)

Goal: a menu action that recovers a bad bonding state and lets the user re-pair, WITHOUT touching the OS bond.

### Honest capability assessment (READ THIS -- it shapes the whole part)

CoreBluetooth on macOS provides **NO API to remove an OS-level pairing/bond programmatically**. There is no
`removeBond`, no `forgetPeripheral`. The encrypted GATT bond lives in the OS Bluetooth database and can only
be cleared by the user via System Settings > Bluetooth ("Forget This Device") or `blueutil`/Control-clicking.
So "forget device" CANNOT mean "delete the bond." What it CAN realistically mean app-side:

- Drop the hub's in-memory link: cancel the connection, nil the cached `peripheral`, clear rx/tx/inbound.
- Restart scanning from a clean slate so the user can reconnect/re-pair.
- Reset the `hadConnection` latch so the UI shows "Searching" (honest fresh start) rather than
  "Disconnected -- reconnecting" (which implies a known prior device).

Crucially, the hub holds **no persisted device identity** today (scan is name-prefix; no UserDefaults/retrieved
id). So there is nothing durable on the HUB side to forget beyond the live link -- which is exactly what
`handleDisconnect()` already tears down. The honest UX is: "reset the connection + rescan" PLUS user guidance
that a truly stuck bond (e.g. encryption keys mismatched after a firmware re-flash) needs the OS-level
"Forget This Device" in System Settings -- which we should deep-link to, not pretend to perform.

### Steps

1. **`BeaconCentral.swift` -- add an explicit app-side reset entry point.**
   Add a public method (runs on `queue`, like `send`):
   ```
   // Forget device / re-pair (issue #16): app-side reset ONLY. CoreBluetooth cannot remove the OS-level
   // bond (no such API) -- a stuck encryption bond still needs the user's System Settings "Forget This
   // Device". This cancels our live link, drops the cached peripheral, resets the reconnect latch, and
   // rescans so the device can be rediscovered/re-paired from a clean state.
   func forgetAndRescan() {
       queue.async { [weak self] in
           guard let self else { return }
           self.hadConnection = false   // honest "Searching" (not "reconnecting" to a device we just forgot).
           self.handleDisconnect()       // cancels connection, nils peripheral/rx/tx, clears inbound, beginScan().
       }
   }
   ```
   WHY reuse `handleDisconnect()`: it is the proven teardown+rescan path; do not duplicate its careful
   cancel-before-nil ordering (the comment at line 100 explains why cancel-first matters so the device
   re-advertises). The only addition is resetting `hadConnection` so `beginScan()` emits `.searching`.
   Note: `handleDisconnect` is currently `private`; keep it private and call it from `forgetAndRescan`
   (same type) -- no visibility change needed.

   Edge: if NOT currently connected (peripheral already nil), `handleDisconnect` is still safe
   (`cancelPeripheralConnection` no-ops, `beginScan` guarded by `poweredOn`). So the action works
   whether or not a device is live.

2. **`MenubarController.swift` -- add the menu action.**
   - `var onForgetDevice: (() -> Void)?` (closure-callback pattern).
   - Add an item near Setup...: `NSMenuItem(title: "Forget device / re-pair", action: #selector(forgetDevice), keyEquivalent: "")`
     with `target = self`; `@objc private func forgetDevice() { onForgetDevice?() }`.
   - Because the action implies user-visible disruption, NOT gating it behind a confirm keeps parity with the
     app's other one-click actions; but it should set a transient status. Simplest honest approach: after firing,
     rely on the existing `setLink`/render path (BeaconCentral will emit `.searching`) so the menu immediately
     reads "Searching for device..." -- no new transient state machine needed.

3. **`AppDelegate.swift` -- wire it + provide OS-bond guidance.**
   - `menubar.onForgetDevice = { [weak self] in self?.forgetDevice() }`
   - `forgetDevice()`:
     ```
     central.forgetAndRescan()
     // CoreBluetooth cannot clear the OS bond; if rescan can't re-establish (keys stale after a re-flash),
     // the user must Forget This Device in System Settings. Surface it as actionable guidance, not a silent no-op.
     menubar.setAlert("Re-pairing... if it won't connect, Forget This Device in Bluetooth settings")
     ```
   - The existing `refreshLink` already clears `alert` on the next `.connected`, so the guidance self-clears on
     a successful re-pair. (Confirm: `refreshLink` calls `menubar.setAlert(nil)` on connected -- yes, line 123.)
   - Optionally add a fix-link to `SettingsLinks.bluetooth` (already used for bluetoothOff) so the user can jump
     to Bluetooth settings -- reuse `SettingsLinks`, no new deep link.

### Verification (Part 3)
- Manual happy path: connected device -> click "Forget device / re-pair" -> menu flips to "Searching for
  device...", hub log shows disconnect, device re-advertises and auto-reconnects (no OS dialog needed if the
  bond is still valid). Alert "Re-pairing..." clears on reconnect.
- Manual stuck-bond path: after a firmware re-flash (bond keys changed), clicking the action will rescan but
  fail to subscribe (encryption error in `didUpdateNotificationStateFor` -> `handleDisconnect`). Confirm the
  guidance alert remains and pointing the user to System Settings "Forget This Device" + re-pair actually
  recovers. This is the honest limitation to validate, not engineer around.
- Confirm no OS-level API was used to remove a bond (code review: only `cancelPeripheralConnection` + rescan).

---

## Cross-cutting / sequencing

- Parts are independent; implement in any order. Suggested: Part 2 (drain) first (highest correctness value,
  smallest blast radius), then Part 1 (login item), then Part 3 (forget/re-pair).
- New file: `hub/Sources/beacon-hub/LoginItem.swift`. No Package.swift change (ServiceManagement is a system
  framework, auto-linked on import, same as CoreBluetooth/AppKit). No Info.plist change strictly required for
  `SMAppService.mainApp` (it uses the main bundle's existing identity); do NOT add a separate helper/agent
  plist (that is the `SMAppService.agent(plistName:)` path -- not needed for mainApp and out of scope).
- build-app.sh: no change needed for any part. (SMAppService.mainApp needs only a valid bundle, which build-app.sh
  already produces + ad-hoc signs.)
- Style: all new menu items follow the `muteLine`/`onOpenSetup` target-action + closure-callback pattern;
  AppDelegate stays the single owner of side effects; MenubarController stays pure display; bridge changes stay
  queue-confined; comments explain WHY; ASCII only.

## Risks / open questions

1. **Ad-hoc signing vs SMAppService (Part 1) -- PRIMARY RISK.** Registration may land as `.requiresApproval`,
   and the changing ad-hoc cdhash on every rebuild can leave stale/duplicate BTM (Login Items) records -- same
   cdhash family as the deferred Keychain issue. In-scope mitigation: reflect real status honestly, alert points
   to System Settings, document Developer-ID as the real fix. Do NOT auto-clean BTM records. CONFIRM on the
   author's machine whether `register()` throws or silently requires-approval under ad-hoc, and word the menu/alert
   to match observed behavior.

2. **CoreBluetooth OS-bond removal is IMPOSSIBLE programmatically (Part 3).** No API exists. "Forget device" is an
   app-side link reset + rescan + honest guidance to System Settings for a truly stuck bond. The hub also has no
   persisted device id to forget. This is a deliberate scope boundary, not a gap to fill.

3. **Drain flush timing (Part 2) -- now completion-aware (codex must-fix #1/#2 folded in).** Termination is held
   until the deny writes flush (DispatchGroup over per-send `onSent`) or a 1.0 s safety cap, and a `terminating`
   flag denies any prompt arriving mid-drain. Residual: assumes the held hook socket is still open/writable at
   quit; if a future CC closes idle hook connections the send could broken-pipe (the `onSent`/`.contentProcessed`
   still fires, so we don't hang). VERIFY against a live CC that the deny reason actually lands in the TUI on Quit.

5. **Stale `CBPeripheralDelegate` callbacks (Part 3, codex nice-to-have).** After `forgetAndRescan` nils
   `peripheral`, a late delegate callback for the OLD peripheral could still mutate `rx`/`tx` (the delegate methods
   don't guard `peripheral == self.peripheral`). Pre-existing and not unique to this issue; add a cheap
   `guard peripheral == self.peripheral` to the rx/tx-mutating delegate methods ONLY if it stays surgical,
   otherwise leave as a noted latent issue (do not expand scope).

4. **`hadConnection` reset semantics (Part 3).** Resetting it to false makes the post-forget UI read "Searching"
   instead of "Reconnecting". Confirm this is the desired honest framing (forgetting => no known prior device).

---

## Critical Files for Implementation
- hub/Sources/beacon-hub/ClaudeCodeBridge.swift  (drain entry point + thread deny message through Pending.respond/finish)
- hub/Sources/beacon-hub/AppDelegate.swift        (applicationShouldTerminate drain handshake, login-item + forget wiring)
- hub/Sources/beacon-hub/MenubarController.swift  (login-item toggle + forget-device menu items)
- hub/Sources/beacon-hub/BeaconCentral.swift      (forgetAndRescan app-side reset)
- hub/Sources/beacon-hub/LoginItem.swift          (NEW: SMAppService.mainApp wrapper)
