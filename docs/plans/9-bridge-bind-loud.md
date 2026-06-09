# Implementation Plan — Issue #9: Make bridge bind failure on port 8765 loud

Hub-only (Swift). Source: audit §3.3.

## 1. Root cause (current file:line; audit's 51/48 drifted)

**Gap A — init-time bind error is stderr-only.** `AppDelegate.swift:54-56` (`startBridge()` catch) writes to stderr only. `NWListener(using:)` (`ClaudeCodeBridge.swift:54`) throws synchronously when 8765 is bound; caught + logged to stderr, never reaches the menu bar. `bridge` stays nil => permission path dead for the whole process with zero UI signal.

**Gap B — async `.failed` ignored.** `ClaudeCodeBridge.swift:59-64` `stateUpdateHandler` handles only `.ready`; `.failed(error)`/`.waiting(error)` fall through silently.

Terminal-state fact: a failed `NWListener` is terminal — cannot `start()` again. Recovery requires creating a fresh listener => `listener` must be a `var` and bind moves out of `init` into a recreatable method.

## 2. Reuse #7's menu-bar alert surface (don't fork a second one)

#7 surface: `MenubarController.alert: String?` (`:17`), `setAlert(_:)` (`:50`), rendered as a pinned top `alertLine` in `render()` (`:59-64`) with hard-coded suffix "-- couldn't show prompt", and `barTitle()` (`:101`) returns "Beacon !" when alert != nil. Producer: `ClaudeCodeBridge.onPromptUndeliverable` (`:21`) wired in `AppDelegate.startBridge()`; cleared on device reconnect (`AppDelegate.swift:81`).

Problem: single `String?` slot + hard-coded prompt suffix. A bridge alert and a prompt alert would clobber each other, and clearing one (reconnect) would wipe the other — reintroducing the silent-failure class #9 fixes.

Minimal generalization: split into two named slots feeding ONE visual surface. Keep `alert` (prompt slot, move its full copy to the call site so no hard-coded suffix), add `bridgeAlert: String?` + `setBridgeAlert(_:)`. `render()` shows `bridgeAlert ?? alert` (bridge priority, safety-critical); `barTitle()` returns "Beacon !" if either non-nil. One alertLine, one bar glyph, two independent producers.

## 3. Ordered surgical edits

**Edit 1 — ClaudeCodeBridge: recreatable listener + status callback.**
- Add `var onBridgeStatus: ((String?) -> Void)?` (non-nil = failed/loud; nil = healthy/recovered), mirroring `onPromptUndeliverable`.
- `private let listener: NWListener` => `private var listener: NWListener?`.
- Remove throwing bind from `init` (init no longer throws); move endpoint/params into `makeListener() throws -> NWListener`.
- `start()` calls new private `bind()`.

**Edit 2 — handle `.failed`/`.waiting` + report (with generation token — codex tweak).** New `bind()` increments a `private var listenerGen: UInt64`, captures it, constructs the listener, sets `newConnectionHandler` + `stateUpdateHandler -> handleState(state, gen)`. Every `handleState`/timer closure carries its `gen` and **early-returns if `gen != listenerGen`** — so a delayed `.waiting`/retry from a superseded listener can never clear/cancel/rebind a newer healthy one (codex: stale-callback guard).
- `.ready` => writePortFile + post `onBridgeStatus(nil)` (CLEAR) + reset backoff + cancel any pending `.waiting` debounce work item.
- `.failed(error)` => `reportFailure` immediately (terminal).
- `.waiting(error)` => `.waiting` is NOT terminal (codex). Do not report/rebind immediately. Schedule a debounce work item (~1s) guarded by `gen`; if still `.waiting` (no `.ready` arrived and gen current) when it fires, THEN `reportFailure`. Cancel the debounce on `.ready`.
- `reportFailure(error)`: guard `gen`; post `onBridgeStatus("Bridge offline - port 8765 in use")` to main, `cancel()`+nil the dead listener, `scheduleRebind()`. Callback hops via `DispatchQueue.main.async` (matches publishBuddy/onClaudeUsage).

**Edit 3 — rebind/retry (no busy-spin).** `scheduleRebind()` uses `queue.asyncAfter` on the existing serial queue with exponential backoff 2s -> 4s -> ... -> 30s cap; reset to 2s on `.ready`. The rebind closure re-checks `gen` before running. One-shot per cycle, no idle spin.

**`allowLocalEndpointReuse` decision (codex):** KEEP it `true` (`ClaudeCodeBridge.swift:51`). It maps to `SO_REUSEADDR`, which only permits rebinding a port in TIME_WAIT (clean hub restart) — it does NOT allow two simultaneously-active listeners on 8765 (that needs `SO_REUSEPORT`), so a real second-instance/conflict still fails and detection is unaffected. Document this rationale inline so a future reader doesn't "fix" it.

**Edit 4 — AppDelegate: init path + wire callback.** `ClaudeCodeBridge()` no longer throws => collapse do/catch. Add `b.onBridgeStatus = { msg in Task { @MainActor in self?.menubar.setBridgeAlert(msg) } }`. Closes Gap A: init bind failure flows bind() -> reportFailure -> onBridgeStatus -> setBridgeAlert. Keep one stderr line in reportFailure for operators (additive).

**Edit 5 — MenubarController: bridge slot + red + clear.** Add `bridgeAlert: String?` + `setBridgeAlert(_:)`. `render()`: bridge first, prompt second, hide if both nil; bridge line as `attributedTitle` (`NSColor.systemRed`) for "loud" (no full bar-item retheme — Tier-1 #5). `barTitle()`: "Beacon !" if `bridgeAlert != nil || alert != nil`.
- **Reused-NSMenuItem attributedTitle leak (codex tweak):** the `alertLine` item is reused. On EVERY `render()` branch explicitly set the item's title state: when showing the bridge alert set `attributedTitle`; when showing the prompt alert or hiding the line, set `alertLine.attributedTitle = nil` (and `.title`/`.isHidden` as needed) so stale red attributed text can't leak into the prompt-alert or hidden state.

## 4. Recovery semantics
- SET on: init `makeListener()` throw, async `.failed(error)`, async `.waiting(error)` (deferred conflict). Each: post loud msg, cancel+nil listener, schedule backoff rebind.
- CLEAR on: first `.ready` of a fresh listener => post `onBridgeStatus(nil)` + reset backoff.
- Retry: `queue.asyncAfter` backoff recreates listener each cycle; occupied port => immediate re-fail (idempotent re-post, render no-op) => next backoff; port frees => `.ready` clears + resets.

## 5. Verify
- `cd hub && swift build && swift test` — compile + regression guard only (bridge/menubar live in the un-testable `beacon-hub` executable target; tests cover BeaconHubKit).
- Manual 1 (Gap A init bind): occupy 8765 before launch — `python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',8765)); s.listen(); print('holding'); input()"` (NO SO_REUSEADDR — codex: keep the conflict unambiguous) — then `./build-app.sh` + launch; expect bar "Beacon !" + red "! Bridge offline - port 8765 in use".
- Manual 2 (recovery): free the port (Ctrl-C holder); within a backoff cycle bar reverts to "Beacon", alert line gone.

## 6. Risks
- Terminal `.failed` listener: must recreate (var + cancel+nil + rebind) or "clears when port frees" is impossible.
- Two-slot generalization prevents #7/#9 cross-clobber; don't collapse to one slot.
- Backoff prevents retry storm; reset only on `.ready`.
- `.waiting` transient flap at healthy startup: debounce the `.waiting` report ~1s (cancel if `.ready` arrives first) to avoid a false loud flash. The one nuance to get right.
- oauth-poller fallback interaction: out of scope; #9 only makes the bridge failure loud.
- Held prompts on rebind: 25s cap still fail-closes orphans; no extra handling.
