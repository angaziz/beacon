# Issue #15 — Minimal first-run status window (Bluetooth / hooks / paired)

Audit source: `docs/research/2026-06-08-hub-ux-audit.md` §3.1, §4 Tier 2 item 10.

## Goal (minimal, not a wizard)
One AppKit window, three checkmark rows, each with a one-click remediation:
- Bluetooth ✓/✗ → open Bluetooth / Privacy settings (reuse existing deep links).
- Claude Code hooks ✓/✗ → **Install** button that runs the `install-hooks` merge from Swift.
- Device paired ✓/✗ → trigger/await the BLE pair flow (passive: it auto-scans; window just reflects + nudges).

Acceptance: live status for all three, one-click fix per row, non-author reaches a working setup without hand-editing JSON.

---

## Grounding (current code — what exists, what's missing)

### Bluetooth state — ALREADY MODELED, reuse it
`BeaconCentral` maps `CBManager.state` → `LinkPhase` (`BeaconCentral.swift:7-15,126-133`). `AppDelegate.refreshLink(_:)` (`AppDelegate.swift:79-96`) is the single sink that converts `LinkPhase` → `MenubarController.Link` and is already called on every phase change. **Do not add a second CBCentralManager** — that would re-trigger TCC and duplicate scanning. The window must be driven from the SAME phase stream AppDelegate already owns.

Mapping of phases to the two rows:
- Bluetooth row OK  ⇔ phase is NOT `.bluetoothOff` / `.unauthorized` / `.unavailable` (i.e. powered + authorized).
- Paired row OK     ⇔ phase is `.connected(name)`. (No separate bond store exists; `connected` = TX-notify subscribed = OS pairing succeeded — see `BeaconCentral.swift:182-196` + comment at `:20`. This is the only available pairing signal; treat "currently connected" as "paired". Open question Q3.)

### Hooks installed — NO MARKER EXISTS, must detect by reading settings.json
Install logic is `build-app.sh install-hooks` (bash + jq, `build-app.sh:18-99`). It merges `claude-code-settings.snippet.json` into `~/.claude/settings.json`: adds the beacon http hooks (inner object `{type:"http", url:"http://127.0.0.1:8765/hook"}`) and wraps `statusLine.command` with the absolute shim path `hub/statusline-shim/beacon-statusline`.

"Installed" is therefore detectable in Swift by reading `~/.claude/settings.json` and checking (CODEX MUST-FIX #2 — tighten; "any beacon URL anywhere" is too weak and can read OK without the essential permission hook). The snippet comment marks `PermissionRequest` + `statusLine` as ESSENTIAL; the rest are optional. So require BOTH:
1. the **`PermissionRequest`** event has a wrapper whose inner hook has `url == "http://127.0.0.1:8765/hook"`, AND
2. `.statusLine.command` contains the shim's absolute path (see install path below).

### Pairing — passive, no explicit trigger API
`BeaconCentral` auto-scans whenever powered on (`beginScan`, `:88-93`) and OS-mediated pairing fires the moment it touches the encrypted TX char (`:176-177`). There is no "start pairing" method and none is needed. The paired row's remediation is informational ("Power on the device and tap to confirm the code") plus, when stuck in `.unauthorized`/`.bluetoothOff`, defer to the Bluetooth row. No new BLE code.

### Conventions to match
- Pure AppKit, `@MainActor`, `NSObject` + target/action (see `MenubarController.swift`). No SwiftUI.
- Persistence via `UserDefaults.standard` (already used for mute, `MenubarController.swift:53-56`).
- Window must be `.accessory`-app friendly: app has `LSUIElement`/`.accessory` (`main.swift:7`, `Info.plist:20`), so to show a real window we must `NSApp.activate(ignoringOtherApps: true)` and the window itself must be able to become key (`NSApp.setActivationPolicy` stays `.accessory`; an ordinary titled `NSWindow` can still be shown and focused from an accessory app — Q1).

---

## Design

### New type: `FirstRunWindowController` (`hub/Sources/beacon-hub/FirstRunWindowController.swift`)
`@MainActor final class FirstRunWindowController: NSObject` owning a programmatically-built `NSWindow` (no nib). Mirrors `MenubarController`'s hand-built style.

State it renders (pushed in by AppDelegate, same pattern as menubar):
```
enum RowState { case checking, ok, bad }   // checking = neutral glyph until first resolve (CODEX nice-to-have #1)
```
Three rows, each = checkmark `NSImageView` (SF Symbol `checkmark.circle.fill` green / `xmark.circle.fill` secondary) + title `NSTextField` + a trailing action `NSButton` (hidden when row is `.ok`).

Public API (called from AppDelegate, main thread):
- `func setBluetooth(_ s: RowState)`
- `func setHooks(_ s: RowState)`
- `func setPaired(_ s: RowState)`
- `func showIfNeeded()` / `func show()` — builds window lazily, centers, `NSApp.activate(...)`, `window.makeKeyAndOrderFront(nil)`.
- callbacks out (set by AppDelegate): `onInstallHooks: (() -> Void)?`, `onOpenBluetoothSettings: (() -> Void)?` (or just open URLs directly inside the controller — they need no app state; lean: open URLs in-controller, keep only `onInstallHooks` as a callback since install touches files + must refresh state).

Row remediation buttons:
- Bluetooth row "Open Settings…": opens `x-apple.systempreferences:com.apple.BluetoothSettings` (BT off) or `...?Privacy_Bluetooth` (unauthorized). Reuse the exact URLs + fallback from `MenubarController.openLink` (`MenubarController.swift:128-138,223-228`). Consider extracting those two URLs to a shared helper to avoid divergence (Q4).
- Hooks row "Install": calls `onInstallHooks?()`; AppDelegate runs the installer, then re-checks and calls `setHooks(...)`.
- Paired row (CODEX MUST-FIX #3): this is the ONE documented exception with no remediation button — pairing is OS-mediated and passive (auto-scans, OS prompts on TX touch). Show an informational hint label instead ("Power on the device; macOS will prompt to pair"). Auto-flips to ✓ when `.connected` arrives. Row title should read live-reachability ("Device connected"), NOT imply a durable bond — `.connected` ≠ persistent paired state (Q3).

Footer (CODEX MUST-FIX #4): a "Don't show again" `NSButton` (checkbox) is the ONLY thing that sets `BeaconFirstRunComplete` while rows are still bad. Plain window-close does NOT set the flag (an incomplete setup must not silently disappear forever). The flag is also set automatically when all three rows reach `.ok`.

### First-run detection (`UserDefaults`)
New key `"BeaconFirstRunComplete": Bool`. Window auto-shows on launch when the flag is false. CODEX MUST-FIX #4: flag is set true ONLY when (a) all three rows reach `.ok`, or (b) the user ticks "Don't show again". Plain close does NOT set it. After the flag is set, the window is reachable via a new menubar "Setup…" item. Matches the existing `UserDefaults` convention; no new file format.

Row states include a transient `checking` (CODEX nice-to-have #1): rows start `.checking` (neutral glyph) until the first `LinkPhase`/hooks read resolves them, so we don't flash a red ✗ before state is known.

### Hooks detection + install in Swift
Two new helpers. Keep them tiny and in one file: `hub/Sources/beacon-hub/HooksInstaller.swift` (`enum HooksInstaller`).

`static func isInstalled(shimPath: String) -> Bool`:
- Read `~/.claude/settings.json`; `JSONSerialization` → dict.
- true iff (a) any `.hooks[event][].hooks[]` has `url == "http://127.0.0.1:8765/hook"` AND (b) `(.statusLine.command as String)` contains `shimPath`.
- Any read/parse failure → false (treat as not installed).

`static func install() throws` — DECISION (Q5): do NOT reimplement the jq merge in Swift. Instead **shell out to the existing `build-app.sh install-hooks`**, the single source of truth for the (non-trivial, idempotent, backup-making) merge. Run via `Process`.

**Shim path — CODEX MUST-FIX #1 + #5 (load-bearing).** The bundle path `Beacon Hub.app/Contents/Resources/` contains a space; `build-app.sh:23` derives `SHIM` from `$PWD` and only *warns* on spaces (`:26-28`), then writes an unquoted `statusLine.command` that won't parse at the CC layer. AND build-app.sh ignores any Swift-passed path (derives its own), so detection and install could disagree. Fix BOTH with one change:
- **Stable no-space install path:** `~/.beacon/beacon-statusline`. On install, Swift copies the bundled shim there (`chmod +x`), then runs `build-app.sh install-hooks` with env `BEACON_SHIM=~/.beacon/beacon-statusline`.
- **`build-app.sh` change:** `SHIM="${BEACON_SHIM:-$PWD/statusline-shim/beacon-statusline}"` (env override, dev fallback unchanged). Dev `./build-app.sh install-hooks` still works as before; the app supplies a no-space path. This makes install and detection use the EXACT same path.
- **Detection** (`isInstalled`) uses the same `~/.beacon/beacon-statusline` as the expected statusLine substring.
- **Bundling:** extend `build-app.sh` packaging (`:108-117`) to copy `build-app.sh`, `claude-code-settings.snippet.json`, and `statusline-shim/beacon-statusline` (keep +x) into `Contents/Resources/`. Swift resolves the bundled `build-app.sh`/shim via `Bundle.main`, dev-fallback to repo path. Without this the Install button is dev-only. (Q6/Q7.)
- run `Process` with `arguments: ["install-hooks"]` + the `BEACON_SHIM` env, capture exit code + stderr; on non-zero surface a one-line error in the window (e.g. "Install failed — jq missing? brew install jq", matching `build-app.sh:20`). jq dependency is a real failure mode to message (Q8).
- **CODEX MUST-FIX #6:** on success, the window shows the restart note ("Restart Claude Code for hooks to take effect", `build-app.sh:97`) so "installed" isn't mistaken for "active in current sessions".

### Wiring in AppDelegate (`hub/Sources/beacon-hub/AppDelegate.swift`)
- Add `private let firstRun = FirstRunWindowController()`.
- In `applicationDidFinishLaunching` (`:28-36`): set `firstRun.onInstallHooks = { [weak self] in self?.installHooksAndRefresh() }`; compute initial hooks state via `HooksInstaller.isInstalled(...)` and `firstRun.setHooks(...)`; call `firstRun.showIfNeeded()` (gated on the UserDefaults flag).
- In `refreshLink(_:)` (`:79-96`): after the existing menubar update, also derive + push BT row and paired row:
  - BT: `.bluetoothOff/.unauthorized/.unavailable` → `.bad`, else `.ok`.
  - paired: `.connected` → `.ok`, else `.bad`.
  This gives **live updates for free** — `refreshLink` already fires on every phase change; no new polling for BT/paired.
- Hooks live-refresh: hooks state only changes when the user clicks Install (or edits settings out-of-band). Re-check on: (a) Install completion, (b) `applicationDidBecomeActive` (cheap, covers manual edits / re-show). Add `func applicationDidBecomeActive(_:)` calling `firstRun.setHooks(HooksInstaller.isInstalled(...))`. No timer needed (keeps it minimal).
- `installHooksAndRefresh()`: `try? HooksInstaller.install(...)`, then `firstRun.setHooks(HooksInstaller.isInstalled(...))`; on throw, tell the window to show the inline error.

### Menubar entry point (so it's reachable after first run)
Add one item to `MenubarController` menu (near Quit, `MenubarController.swift:83-84`): "Setup…" → target/action calling a new `onOpenSetup` closure that AppDelegate wires to `firstRun.show()`. Keeps the window reachable once `BeaconFirstRunComplete` is set. (Minimal addition; matches the existing mute/quit item pattern.)

---

## Ordered, verifiable steps

1. **HooksInstaller.swift (new)** — `hub/Sources/beacon-hub/HooksInstaller.swift`. Add `enum HooksInstaller`:
   - `shimInstallPath` = `~/.beacon/beacon-statusline` (no-space, stable).
   - `isInstalled(settingsURL:shimPath:) -> Bool` core (path-injectable, Q9): JSONSerialization read; require **`PermissionRequest`** wrapper with inner `url=="http://127.0.0.1:8765/hook"` AND `.statusLine.command` contains `shimPath`. Any read/parse failure → false. Public convenience defaults `settingsURL=~/.claude/settings.json`, `shimPath=shimInstallPath`.
   - `install() throws`: mkdir `~/.beacon`, copy bundled shim → `shimInstallPath` (`chmod 0755`), then `Process` runs bundled `build-app.sh` `["install-hooks"]` with env `BEACON_SHIM=shimInstallPath`; throw `HooksError` carrying stderr on non-zero exit.
   - Pure-detection logic (dict → Bool) may live in BeaconHubKit for unit testing; file-read + Process stay in the executable (Q9).
   - Verify: `swift build -c debug` compiles; table-driven unit test of the pure detector in `BeaconHubKitTests` (cases: missing file, no PermissionRequest, PermissionRequest-but-no-statusline, fully-installed, statusline-without-shim).

2. **FirstRunWindowController.swift (new)** — `hub/Sources/beacon-hub/FirstRunWindowController.swift`. Programmatic `NSWindow` (titled, closable, ~360×220), `NSStackView` of three rows, each `[NSImageView checkmark][NSTextField title][NSButton action]`. Public `setBluetooth/setHooks/setPaired(_:RowState)`, `show()/showIfNeeded()`, `onInstallHooks`, `onOpenSetup` not needed here. Render hides the action button on `.ok`. Install button calls `onInstallHooks`. BT button opens the settings URL in-controller (reuse `MenubarController`'s URLs). Footer dismiss + `BeaconFirstRunComplete` flag handling.
   - Verify: build; manually `./build-app.sh run` and confirm window appears, rows render, buttons present.

3. **AppDelegate wiring** — edit `hub/Sources/beacon-hub/AppDelegate.swift`:
   - add `firstRun` property;
   - in `applicationDidFinishLaunching`: wire `onInstallHooks`, seed hooks state, `showIfNeeded()`;
   - extend `refreshLink(_:)` to push BT + paired row states;
   - add `applicationDidBecomeActive(_:)` to re-check hooks;
   - add `installHooksAndRefresh()`.
   - Verify: toggle Bluetooth off/on and watch BT row flip; connect device and watch paired row flip ✓.

4. **MenubarController "Setup…" item** — edit `MenubarController.swift`: add an `onOpenSetup: (() -> Void)?` + a menu item + `@objc` action; AppDelegate sets it to `firstRun.show()`.
   - Verify: menu shows "Setup…", clicking re-opens the window.

5. **Packaging + BEACON_SHIM override** — edit `build-app.sh`:
   - install-hooks: `SHIM="${BEACON_SHIM:-$PWD/statusline-shim/beacon-statusline}"` (CODEX MUST-FIX #1/#5; dev behaviour unchanged when env unset).
   - packaging block (`:108-117`): copy `build-app.sh`, `claude-code-settings.snippet.json`, `statusline-shim/beacon-statusline` (keep +x) into `Contents/Resources/`.
   - `HooksInstaller` resolves bundled `build-app.sh` + shim via `Bundle.main`, dev-fallback to repo path.
   - Verify: build `.app`, run from a copied location (outside repo, path WITH a space), click Install, confirm `~/.beacon/beacon-statusline` exists, `~/.claude/settings.json` gains the PermissionRequest beacon hook + `statusLine.command` pointing at the no-space `~/.beacon` path, and a `.bak` backup is made. Confirm the space-warning does NOT fire.

6. **Full end-to-end check** — fresh-ish env: ensure `BeaconFirstRunComplete` unset (window auto-shows); fix each row; confirm flag flips and window can be re-opened via menubar "Setup…".

---

## Risks / open design questions

- **Q1 (accessory window focus):** `.accessory` apps can show ordinary `NSWindow`s but need `NSApp.activate(ignoringOtherApps:true)` to take focus; buttons must remain clickable. Verify the window becomes key. If problematic, a borderless panel is the fallback.
- **Q2 (auto-show policy + dismiss):** Gate auto-show on `BeaconFirstRunComplete` (recommended) vs show-whenever-any-row-bad. Decide whether reaching all-✓ auto-dismisses or just enables close. Recommend: gate on flag, manual close, set flag on close or on all-✓.
- **Q3 (paired == connected):** No persistent bond store exists; "paired" is approximated by current `.connected`. A previously-bonded but currently-off device reads ✗. Acceptable for a status window (it reflects live reachability), but note the semantic gap; a true "bonded" check would need OS Bluetooth introspection (out of scope, matches §3.6 "no forget-bond UX").
- **Q4 (URL duplication):** BT/Privacy deep-link URLs currently live only in `MenubarController`. Extract to a tiny shared helper to avoid drift, or duplicate (2 string constants). Lean: small shared `enum SettingsLinks`.
- **Q5 (install impl):** Shell out to `build-app.sh install-hooks` (recommended — single source of truth, keeps the careful jq merge + backup) vs reimplement the merge in Swift (more code, divergence risk, but no Process/bundling dependency). Recommend shelling out.
- **Q6/Q7 (bundling + path resolution):** Install button only works from the shipped `.app` if `build-app.sh`, the snippet, and the shim are bundled into `Contents/Resources/`. This is the load-bearing change; without it the feature is dev-only. Confirm Process can execute the bundled bash script (it can; ensure +x preserved on copy).
- **Q8 (jq dependency):** `install-hooks` hard-requires `jq` (`build-app.sh:20`). Surface its absence as a clear one-line error in the window ("Install jq: brew install jq").
- **Q9 (testability):** Make `HooksInstaller.isInstalled` core path-injectable (`settingsURL:`) so it can be unit-tested in `BeaconHubKitTests` without touching real `$HOME`. Note `HooksInstaller` lives in the executable target, not BeaconHubKit — if a unit test is wanted, the pure detection logic may belong in BeaconHubKit (parse a dict → Bool), leaving the file-read + Process in the executable.
- **Statusline-replace caveat (audit §3.1):** The installer wraps (not replaces) an existing statusLine, so detecting "hooks installed" via shim-substring is robust to a pre-existing renderer. Good.

## Codex nice-to-haves folded in (do these)
- **#2** re-check hooks in `show()` too (not only `applicationDidBecomeActive`) so the Setup-menu path is always fresh; no timer.
- **#3** run `HooksInstaller.install()` OFF the main thread (or async) and disable the Install button + show "Installing…" while `Process` runs, so the window never appears frozen; re-enable + refresh on completion (hop back to `@MainActor`).
- **#4** extract the Bluetooth/Privacy deep-link URLs into a tiny shared `enum SettingsLinks` reused by both `MenubarController` and `FirstRunWindowController` (avoid string drift).

## Files to touch
- NEW `hub/Sources/beacon-hub/FirstRunWindowController.swift`
- NEW `hub/Sources/beacon-hub/HooksInstaller.swift`
- EDIT `hub/Sources/beacon-hub/AppDelegate.swift`
- EDIT `hub/Sources/beacon-hub/MenubarController.swift`
- EDIT `hub/build-app.sh` (bundle installer assets into Resources)
