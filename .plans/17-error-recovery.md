# Issue #17 -- Error recovery: token-refresh-on-401 self-heal + BLE failure timeout/escalation

Branch: `fix/17-error-recovery`
Source audit: `docs/research/2026-06-08-hub-ux-audit.md` 3.3 (and 4 Tier-1 item 5 for the menu glyph wiring).

Two INDEPENDENT, cleanly-separable parts. Part A is provider-credential plumbing in the poller;
Part B is a bounded-retry escalation in the BLE central + one new menu state. They touch disjoint
files except for the shared menu-render conventions. No speculative flexibility. ASCII only (=> not arrows).

Out of scope (do NOT touch): the §3.3 bridge-bind-failure surface (already shipped #9), the silent
auto-deny (#7), and the device-side state work (#11/#12). This issue is ONLY the 401 self-heal and the
BLE pairing-failed escalation.

---

## Grounding (verified by reading the code + inspecting on-disk credentials)

### Part A -- what refresh material ACTUALLY exists (inspected, not assumed)

- **Claude** Keychain item `service = "Claude Code-credentials"`, generic-password, JSON blob:
  `claudeAiOauth = { accessToken, refreshToken, expiresAt(epoch ms, Int), scopes, subscriptionType, rateLimitTier }`.
  So a **refreshToken AND expiresAt DO exist** in the Keychain blob. `UsagePoller.ClaudeUsageProvider.accessToken()`
  (line 132) today reads ONLY `accessToken` and discards the rest. There is **no `~/.claude/.credentials.json`
  on macOS** -- creds live in Keychain only (verified: file absent).
  - The OAuth **token endpoint + client_id for Claude are NOT present in the stored creds** and not in this
    codebase. They are an Anthropic console OAuth detail. Implementing a real refresh-token grant for Claude
    would require hardcoding an undocumented endpoint + public client_id => fragile, exactly the case the
    issue's HONESTY REQUIREMENT says to avoid. **=> Claude: do NOT plan a guessed network refresh call.**
  - BUT Claude Code itself refreshes the token out-of-band (the running `claude` CLI rotates the Keychain
    item before `expiresAt`). The poller already invalidates its cached token on 401 (line 117) but only
    re-reads on the NEXT 45 s poll and surfaces "re-login" in between. **The achievable self-heal: on 401,
    re-read the Keychain (rotated by the CLI) and retry the request ONCE this same poll.** Covers the common
    case at zero OAuth risk.

- **Codex** `~/.codex/auth.json` (verified keys):
  `{ auth_mode, last_refresh(ISO), OPENAI_API_KEY, tokens:{ access_token, account_id, id_token, refresh_token } }`.
  So a **refresh_token, an id_token, AND an OPENAI_API_KEY fallback all exist on disk**, and `last_refresh`
  is bumped when the CLI rotates. `CodexUsageProvider.credentials()` (line 190) reads only `access_token` +
  `account_id` today.
  - The Codex OAuth refresh endpoint + client_id ARE reliably discoverable -- extracted from the installed
    codex native binary strings (`@openai/codex-darwin-arm64/.../bin/codex`):
      endpoint  = `https://auth.openai.com/oauth/token`
      client_id = `app_EMoamEEZ73f0CkXaXp7hrann`
      grant     = `grant_type=refresh_token` + `refresh_token=<...>` + `client_id=<...>`
      body type = `application/x-www-form-urlencoded`
      response  = `{ access_token, refresh_token, id_token, expires_in, ... }` (standard OAuth token response).
    These are the SAME values the CLI uses, so a refresh grant is feasible for Codex. HOWEVER the safer,
    lower-risk primary path is the same as Claude's: **re-read `auth.json` on 401 (the CLI rotates it in
    place and bumps `last_refresh`) and retry once.** A real refresh grant is the OPTIONAL secondary fallback
    (decision below).
  - The TODO's "local rollout fallback" (D1) = `~/.codex/sessions/**/rollout-*.jsonl` carries `rate_limits`
    lines the CLI wrote during a session. That is a SEPARATE data source (read the most recent rollout's
    rate_limits to fill usage when the network endpoint is unavailable). It is NOT a token-refresh mechanism.
    Treat it as out-of-scope-but-noted for #17 (it is a usage fallback, not error recovery) UNLESS we want a
    last-resort usage source on persistent 401; recommend deferring to keep #17 surgical (open question Q4).

### Part A -- poll-loop structure

- `UsagePoller.poll()` (line 49) fires every 45 s on the private `beacon.usage` queue, runs the two
  providers concurrently in a `DispatchGroup`, and merges to `Usage` + `[String]` errors -> main thread.
- Each provider's `fetch(completion:)` is a single `URLSession.dataTask`. A 401 branch already exists in
  both (Claude line 116, Codex line 176). **A single-retry-on-401 fits cleanly INSIDE `fetch` with no
  re-architecture**: on 401, refresh credentials, and if they changed, re-issue the request ONCE; only then
  surface the "re-login" reason. The retry must be bounded to ONE to avoid loops, and must not recurse
  forever if the rotated token is still 401.
- `ClaudeUsageProvider` caches the token behind `cachedToken` + `NSLock` and clears it on 401 (line 91).
  The retry re-reads via the SAME `accessToken()` path -- no new Keychain prompt logic needed (the cache
  is already cleared before re-read).

### Part B -- BLE failure paths (verified)

- `BeaconCentral` (queue `beacon.central`). `LinkPhase` (line 7) cases: bluetoothOff, unauthorized,
  unavailable, searching, connecting(String), connected(String), reconnecting. `setPhase` (line 48)
  dedupes + emits `onPhaseChange` on the queue.
- EVERY BLE failure funnels into `handleDisconnect()` (line 107) => `beginScan()` (line 100) =>
  `setPhase(hadConnection ? .reconnecting : .searching)`. **There is NO attempt counter and NO timeout** --
  bonding can loop forever silently. Failure call sites: `didFailToConnect` (163), `didDisconnectPeripheral`
  (168), `didDiscoverServices` fail (177), `didDiscoverCharacteristics` fail (185), `didUpdateNotificationStateFor`
  encryption/pairing fail (200). Success latch: `isConnected = true` in `didUpdateNotificationStateFor`
  (line 206), whose `didSet` (line 36) sets `hadConnection = true` and emits `.connected`.
- `hadConnection` (line 47) is the existing latch distinguishing a FIRST-time pair (false => `.searching`)
  from a reconnect of a previously-good device (true => `.reconnecting`). `forgetAndRescan()` (line 92)
  resets it to false. This is exactly the signal to AVOID escalating on a normal brief reconnect blip.
- Phase flow to UI: `AppDelegate.refreshLink(_:)` (line 193) maps `LinkPhase` -> `MenubarController.Link`
  (1:1 switch, MUST stay exhaustive) and also drives the first-run window's Bluetooth/paired rows.
  `MenubarController` renders `link` in `render()` (statusLine line 151, fixLine remediation line 162,
  glyph in `applyBarIcon()` line 211) and shows/hides `pairLine` (line 188). The `fixLine` is an existing
  target/action remediation row (`openLink`, `fixURL`) -- the escalation's "try again" can reuse this
  exact pattern OR a dedicated action row (decision below). `forgetDevice()` menu action already exists
  (#16) and calls `central.forgetAndRescan()` -- the escalation can point at it for the stuck-bond case.

### Test surface

- Pure logic lives in `BeaconHubKit` (UsageNormalizer, Protocol) with table-driven XCTest in
  `hub/Tests/BeaconHubKitTests/`. `UsagePoller` + `BeaconCentral` are in the `beacon-hub` executable target
  (NOT in the Kit) and are NOT currently unit-tested. To get Part A logic under test, the **pure, network-
  free pieces must be extracted into `BeaconHubKit`** (see Part A step 1) so they can be table-tested like
  UsageNormalizer. Part B's escalation decision (a pure state machine over attempt count / elapsed time /
  hadConnection) should likewise be extracted to a pure helper so it is testable without CoreBluetooth.

---

## Part A -- Token-refresh-on-401 self-heal (Claude + Codex)

Goal: a 401 self-heals within the SAME poll by picking up CLI-rotated credentials and retrying once,
with an optional real refresh-token grant for Codex (feasible) and NONE for Claude (not feasible).

### Decision: re-read-rotated-creds-and-retry ONLY (both providers) -- USER-DECIDED 2026-06-09

The user chose option 1: ship the zero-risk self-heal for BOTH providers, and DEFER any real OAuth
refresh-token grant to a separate follow-up issue. So:
- **Claude AND Codex: re-read-rotated-creds-and-retry-once ONLY.** On a 401, re-read the CLI-rotated
  credentials (Keychain blob for Claude, `~/.codex/auth.json` for Codex) and, if the token actually changed,
  re-issue the request exactly ONCE before surfacing the "re-login" reason.
- **NO network OAuth grant for either provider. NO gated/dead code.** Do NOT add `allowNetworkRefresh`, do
  NOT add `parseCodexTokenResponse`, do NOT hardcode any reverse-engineered endpoint/client_id. The real
  refresh grant (Codex params verifiable; Claude params appear extractable from the CLI binary but both are
  unofficial/fragile) is a SEPARATE follow-up -- out of scope for #17.

This satisfies Acceptance ("A 401 triggers a token refresh and self-heals without manual re-login, Claude +
Codex") for the normal case (an active user whose CLI has rotated the token). Honest limitation: a fully
idle, truly-expired token still ends at "re-login" -- recorded in Risks, deferred to the follow-up.

### Steps

1. **New file `hub/Sources/BeaconHubKit/ClaudeCredentials.swift` and `CodexCredentials.swift`** (pure,
   Foundation-only, public, NO Keychain / NO URLSession) -- OR a single `ProviderCredentials.swift`
   holding both. Move the *parsing* out of `UsagePoller` so it is table-testable:
   - `public static func parseClaude(_ blob: Data) -> String?` -- returns the `claudeAiOauth.accessToken`
     (the only field option-1 uses). WHY a pure parser: the Keychain READ stays in the executable (needs
     Security.framework), but the JSON SHAPE is what breaks when the CLI changes its blob -- isolate + test it.
     Do NOT parse refreshToken/expiresAt: they are unused under option 1 (no real grant), so parsing them
     would be speculative.
   - `public static func parseCodex(_ json: Data) -> (accessToken: String, accountId: String)?` -- parses
     `tokens.{access_token,account_id}` (the only fields used). Same "isolate the shape" rationale. Do NOT
     parse refresh_token / OPENAI_API_KEY -- unused under option 1.
   (NO `parseCodexTokenResponse` -- the real OAuth grant is deferred, so there is no token-response to parse.)
   Keep them tiny and dependency-free; mirror `UsageNormalizer`'s "expect breakage, isolate the shape" doc.

2. **`UsagePoller.ClaudeUsageProvider`** -- thread the refresh-on-401:
   - Replace `accessToken()` (line 132) to read the Keychain blob and return via `ClaudeCreds.parseClaude`,
     caching the `accessToken` as today (keep the one-prompt-per-run cache + `invalidateToken()`).
   - In `fetch`'s 401 branch (line 116): keep `invalidateToken()`, then **re-read the token once**; if the
     freshly-read token DIFFERS from the one that just got 401'd, re-issue the request ONCE with the new
     token and complete on its result. Only if the token is unchanged (CLI has not rotated) OR the retry
     also 401s, surface the existing reason. Signature (private helper, no recursion):
     `private func fetch(token: String, retryOn401: Bool, completion: ...)` -- the public `fetch` calls it
     with `retryOn401: true`; the retry calls with `retryOn401: false`. WHY a bool not a counter: exactly
     one retry, structurally impossible to loop.
   - Reason string on terminal failure unchanged ("Claude token expired - re-login") so existing menu copy
     and the `rebuildUsage` claude-error filtering (AppDelegate line 262) keep working.

3. **`UsagePoller.CodexUsageProvider`** -- thread the refresh-on-401:
   - Replace `credentials()` (line 190) to return `CodexCreds` via `CodexCredentials.parseCodex` (so
     `refresh_token` + `OPENAI_API_KEY` become available). Keep the existing 2-tuple call sites working by
     reading `.accessToken`/`.accountId`.
   - In the 401 branch (line 176): primary = **re-read `~/.codex/auth.json`** (the CLI rotates it + bumps
     `last_refresh`); if `access_token` changed, retry the wham/usage request ONCE with the new token.
     Same `retryOn401: Bool` one-shot structure as Claude.
   - NO network OAuth grant (user-decided: deferred). Codex gets the same re-read-and-retry-once shape as
     Claude and nothing more. The `refreshToken`/`apiKey` fields parsed in step 1 are still useful context
     but are NOT used to make a refresh call in #17.
   - Terminal reason unchanged ("Codex token expired - re-login").

4. **Wire-up sanity:** no change needed in `UsagePoller.poll()` / `AppDelegate` -- the retry is internal to
   each provider's `fetch`. The 45 s cadence, DispatchGroup merge, and error surfacing are untouched.

### Verification (Part A)

- `swift build` (compile) + `swift test` for the new `BeaconHubKitTests`:
  - **Table-driven** `ClaudeCredentialsTests` / `CodexCredentialsTests`: valid blob -> accessToken
    (Claude) / (access,account) (Codex); malformed JSON -> nil; missing the required field -> nil; empty
    string token -> nil (treat as absent).
- Manual: run the app with an expired-but-CLI-rotated token (or stub a 401 via a mock `UsageProvider`/
  injected `URLSession`) and confirm the menu recovers to a live pct WITHOUT a manual re-login, within ONE
  poll, and that a genuinely-dead token still ends at the "re-login" reason (no infinite retry).
- Grep for "TODO(P2-0)" in `UsagePoller.swift` -> both removed.

---

## Part B -- BLE failure timeout / escalation ("pairing failed, try again")

Goal: bonding cannot loop forever silently. After N consecutive failed connect/bond attempts OR T seconds
without reaching `.connected` on a FIRST-time pair, escalate to a loud `.pairingFailed` state with a
"Try again" affordance; do NOT escalate on a brief reconnect blip of a previously-good device.

### Decision: counter vs timeout, and where it lives

RECOMMENDATION: use BOTH a consecutive-attempt counter AND a wall-clock deadline, whichever trips first,
because some failures (immediate `didFailToConnect`) burn attempts fast while others (waiting on a pairing
dialog the user ignores) burn time. Keep ALL of it queue-confined in `BeaconCentral` (the only safe place;
phases are already computed on `beacon.central`). Extract the pure decision into a testable helper.

Escalate ONLY when `hadConnection == false` (first-time pair). A previously-good device that drops keeps
the existing `.reconnecting` behavior indefinitely (that is correct -- it auto-heals when back in range).
WHY: the audit explicitly wants to distinguish first-time pairing failure from a transient reconnect blip,
and `hadConnection` is the existing, verified latch for exactly that.

### Steps

1. **New pure helper, in `BeaconHubKit`** (`PairingEscalation.swift`) so it is unit-testable without
   CoreBluetooth:
   - `public struct PairingEscalation { public init(maxAttempts: Int, deadline: TimeInterval) }`
   - `public mutating func recordAttemptStart(now: TimeInterval)` -- stamps the first-attempt time if unset.
   - `public mutating func recordFailure(now: TimeInterval, hadConnection: Bool) -> Bool` -- increments the
     counter; returns `true` (=> escalate) when `!hadConnection && (attempts >= maxAttempts || now - firstAt >= deadline)`.
   - `public mutating func reset()` -- clears counter + firstAt (called on success and on "try again").
   Pure value type, no timers. WHY: the escalation rule is the load-bearing logic; testing it table-driven
   (attempt sequences, elapsed times, hadConnection true/false) is where bugs hide.

2. **`BeaconCentral.swift`** -- new LinkPhase + wire the helper:
   - Add `case pairingFailed` to `LinkPhase` (line 7). Keep it Equatable (no associated value needed; the
     name shown is the prefix "Beacon", and the device is unpaired by definition here).
   - Add `private var escalation = PairingEscalation(maxAttempts: 4, deadline: 25)` (queue-confined). The
     numbers are a starting point -- see Risks Q for tuning. 4 attempts / 25 s comfortably exceeds a single
     real OS pairing-dialog round-trip but bounds a genuinely-stuck bond.
   - In `handleDisconnect()` (line 107): BEFORE `beginScan()`, call
     `if escalation.recordFailure(now: ..., hadConnection: hadConnection) { setPhase(.pairingFailed); return }`
     -- i.e. when the helper says escalate, emit `.pairingFailed` and DO NOT re-scan (stop the silent loop).
     Otherwise fall through to `beginScan()` as today. Use `DispatchTime.now()` / a monotonic clock source
     for `now` (queue-confined; document WHY monotonic: wall-clock jumps must not trip the deadline).
   - In `beginScan()` (line 100): call `escalation.recordAttemptStart(now:)` so the deadline clock starts
     at the first scan of a fresh pairing attempt sequence.
   - On success (`isConnected` didSet rising edge, line 41): `escalation.reset()` so a later disconnect of
     a now-good device never escalates (it becomes `hadConnection == true` reconnect, which the helper
     never escalates anyway, but reset keeps the counter honest for any future first-pair).
   - In `forgetAndRescan()` (line 92): `escalation.reset()` too (the user explicitly asked for a clean
     re-pair; give it a full fresh budget).
   - New public entry for the menu's "Try again": `func retryPairing()` -- on the queue: `escalation.reset()`
     then `beginScan()` (resumes scanning from a clean state). WHY separate from `forgetAndRescan()`:
     "try again" should NOT pop the System-Settings "Forget This Device" modal; it just resets the budget
     and rescans. `forgetAndRescan()` remains the heavier stuck-bond path.

3. **`AppDelegate.refreshLink(_:)`** (line 193) -- extend the exhaustive switch with
   `case .pairingFailed: link = .pairingFailed`. Keep the `connected` computation false for it. For the
   first-run window mapping (line 213): `.pairingFailed` => Bluetooth `.ok` (BT is fine; it is pairing that
   failed) and paired `.bad`. Add `central.retryPairing` wiring: set `menubar.onRetryPairing = { [weak self]
   in self?.central.retryPairing() }` in `startCentral()` (or alongside `onForgetDevice` in `startLoginItem`).

4. **`MenubarController.swift`** -- render the new state loudly + a "Try again" action:
   - Add `case pairingFailed` to `enum Link` (line 9).
   - `render()` statusLine (line 151): `case .pairingFailed: statusLine.title = "Pairing failed"`.
   - Reuse the `fixLine` remediation row pattern (line 162) OR add a dedicated actionable row. RECOMMEND a
     dedicated `retryLine` (target/action, like `muteLine`/`forgetLine`) titled "Try pairing again" shown
     ONLY for `.pairingFailed`, calling `onRetryPairing`. WHY not reuse `fixLine`: `fixLine` opens a URL
     (Settings) via `openLink`; "try again" is an in-app action, not a deep link -- a separate item keeps
     the target/action clean and lets the existing forget-device row stay the System-Settings escape hatch.
   - Glyph in `applyBarIcon()` (line 211): add `case .pairingFailed: symbol = "exclamationmark.triangle.fill";
     tint = .systemOrange; description = "Beacon: pairing failed"`. WHY orange not red: it is recoverable
     (try again), matching the bluetoothOff/unauthorized orange convention; red is reserved for the
     bridge/alert safety surface.
   - `pairLine` visibility (line 188): include `.pairingFailed` so the "enter the code shown on the device"
     hint stays relevant while the user retries. (Optional -- decide with Q3.)
   - Add `var onRetryPairing: (() -> Void)?` and the `@objc func retryPairing()` selector, mirroring
     `forgetDevice()` exactly.

### Verification (Part B)

- `swift test` -- **table-driven** `PairingEscalationTests`: sequences of `recordFailure` with
  `hadConnection:false` trip at `maxAttempts`; a slow single attempt trips at `deadline`; ANY
  `hadConnection:true` failure NEVER escalates; `reset()` (success / try-again / forget) clears the budget;
  attempts below threshold + under deadline do NOT escalate.
- `swift build` -- confirm the LinkPhase/Link switches stay exhaustive (compiler enforces both the
  `refreshLink` map and the `applyBarIcon`/`render`/first-run switches handle `.pairingFailed`).
- Manual: power the device off (or hold it out of bonding range) on a FRESH pair (after Forget device);
  confirm the menu escalates from "Searching..." to "Pairing failed" + orange glyph + "Try pairing again"
  within ~25 s / 4 attempts, that "Try again" resumes scanning, and that a NORMAL disconnect of an
  already-paired device still shows "Disconnected -- reconnecting" and never escalates.

---

## Risks / Open questions

- **Q1 (Part A, Claude OAuth feasibility -- DECIDED).** Claude's refresh endpoint/client_id are NOT
  discoverable from the Keychain blob or this codebase. A real refresh grant would require a hardcoded
  undocumented endpoint+client => fragile. DECISION: Claude gets re-read-rotated-creds-and-retry ONLY, no
  network refresh. This satisfies the acceptance criterion for the normal "expired but still logged in"
  case (the CLI rotates the Keychain). RISK: if the user's `claude` CLI is NOT running/has not rotated,
  the 401 still ends at "re-login" -- acceptable and honest; a true self-refresh isn't safely buildable.

- **Q2 (Part A, real OAuth grant) -- DECIDED: DEFERRED.** The user chose option 1 (re-read + retry only).
  The real refresh-token grant for Codex (params `auth.openai.com/oauth/token`,
  `client_id=app_EMoamEEZ73f0CkXaXp7hrann`, lifted from the installed binary -- unofficial) and for Claude
  (params appear extractable from the `claude` CLI binary but are likewise unofficial) is OUT OF SCOPE for
  #17 and should be filed as a separate follow-up issue. HONEST LIMITATION shipped in #17: a fully idle,
  truly-expired token (CLI not running to rotate it) still ends at "re-login"; the re-read path only
  self-heals once the CLI has rotated the credential out-of-band, which is the common case.

- **Q3 (Part B, pairLine on pairingFailed).** Should the "enter the code shown on the device" hint stay
  visible in the failed state? Pro: still relevant during retry. Con: if the failure is "device asleep/out
  of range" the code isn't showing. Minor -- recommend showing it.

- **Q4 (Part A, Codex rollout fallback / D1).** `~/.codex/sessions/**/rollout-*.jsonl` `rate_limits` is a
  USAGE fallback, not a token mechanism. Out of scope for #17's error-recovery; noted only because the TODO
  mentioned it. Recommend NOT bundling it into #17 (separate concern, keeps this surgical).

- **Q5 (Part B, threshold tuning).** `maxAttempts = 4`, `deadline = 25 s` are starting points. A real OS
  pairing dialog can sit waiting on the user for longer than 25 s; if the user is mid-typing the passkey we
  must NOT escalate under them. MITIGATION: the deadline clock starts at `beginScan`, and a successful
  `setNotifyValue`/connect resets it; but a slow human passkey entry happens AFTER connect/char-discovery,
  i.e. inside `didUpdateNotificationStateFor` -- verify the encryption-fail path only fires on actual
  failure, not while the dialog is open (it does: the callback arrives with `error != nil` only on
  failure). Still, consider raising `deadline` to ~45 s and/or `maxAttempts` to ~6 after one manual pairing
  trial. NEEDS one real-device tuning pass.

- **Q6 (shared menu render).** Both parts converge on `MenubarController.render`/`applyBarIcon`. They are
  independent edits (Part A adds nothing to the menu; Part B adds one Link case). Land Part A and Part B as
  separate commits to keep them cleanly separable per the constraint.
