# Hub Usage Reliability — Claude 429 Resilience

- Status: Proposed
- Date: 2026-06-25
- Components: `hub` (BeaconHubKit + beacon-hub), `firmware` (records/datastore/codec/ui), `docs` (CONTRACT.md)
- Related: #59 (equality-gate fan-out), #64 (poll gating), #93 (statusline freshness), #17 (401 self-heal)

## Problem

Claude usage intermittently shows `Claude usage unavailable (HTTP 429)` and the value blanks to
`--`. Two verified gaps:

1. **A transient failure wipes last-known-good.** On any non-200 the provider returns
   `ProviderUsage.unavailable` (`pct nil` => `--`); there is no retention anywhere on the hub. A
   few-second 429 blanks the display for the whole gap.
2. **No backoff / no `Retry-After`.** The 45s loop re-hits the rate-limited oauth endpoint every tick
   with identical headers, ignoring `Retry-After`. Fixed-cadence hammering tends to prolong the limit.

The robust source (the statusline shim, #93) already dodges 429s while a Claude Code session is
active. The failure is the **gap case**: no active session => statusline aged out (> 2x the 45s poll
interval) => fall back to the 429-prone oauth endpoint => blank.

## Goals

1. Keep showing the last-known value during a transient 429 (do not blank to `--`).
2. Stop hammering: exponential backoff + jitter, honor `Retry-After`.
3. Reduce baseline poll pressure on the oauth endpoint when idle.
4. Truthful UX: distinguish "rate-limited, showing last value" from a hard error, end-to-end to the
   device.

## Non-Goals (YAGNI)

- **Disk persistence of last-good.** The problem is within-run; the statusline repopulates on the next
  session. Cross-restart retention is deferred.
- Changing the statusline shim, the oauth/Codex endpoints, or the 401 self-heal mechanics.

## Degradation Model

```
 LIVE (statusline or oauth 200)
     │  session ends / oauth 429
     ▼
 STALE (last-good shown, dimmed; while age <= maxStale AND now < served reset)
     │  maxStale elapsed OR window reset passed
     ▼
 UNAVAILABLE ("--")

 token missing/expired/shape-drift ─────────────▶ UNAVAILABLE + actionable banner (clears last-good)
```

Principle: **transient failures (429 / 5xx / network) serve last-known-good as STALE; terminal
failures (auth / response-shape) blank to `--` with an actionable banner.** A 429 never blanks a value
we recently held.

## Architecture

Three concerns, three homes (matches the existing transport/policy split):

| Concern | Home | Why |
|---|---|---|
| Classify transport facts (HTTP code, headers, parse, creds) | **Providers** (`ClaudeUsageProvider`, `CodexUsageProvider`) | only the provider observes URLSession/Keychain/Codex-file state |
| Backoff + oauth-poll gating | **`UsagePoller`** | it owns the timer and decides whether to hit the oauth endpoint |
| Retention + staleness + UX classification | **Pure reducer** in `BeaconHubKit`, driven by `AppDelegate.rebuildUsage` | `rebuildUsage` is the only place that sees BOTH sources (statusline + poller); retention must see the freshest of either |

> Retention is deliberately NOT in the poller: statusline values live in `AppDelegate`
> (`statuslineClaude`), so poller-owned retention could serve older oauth data after statusline expiry.
>
> **Source precedence (not a blind merge).** The reducer is driven by the *authoritative* observation
> per the existing freshness gate, never by a free race of concurrent events: `rebuildUsage` selects
> the active Claude source (statusline if fresh, else the latest oauth outcome) and feeds **that one**
> observation into the reducer. This prevents a late-completing oauth `.transient`/`.terminal` from
> downgrading or clearing a value a fresh statusline POST just established.
>
> **Flat-session aging.** `onStatuslineActivity` fires on every `rate_limits` POST (#93) even when the
> value is unchanged/deduped. It re-affirms the current statusline value as a `.live` observation so
> `lastGoodAt` does not age out during an active-but-flat session.

### 1. Providers return a classified outcome (BeaconHubKit + beacon-hub)

```swift
enum ProviderOutcome: Equatable {
    case live                                                // HTTP 200 + normalized OK
    case transient(retryAfter: TimeInterval?, reason: String)// 429 / 5xx / network — last-good + backoff
    case terminal(reason: String, kind: TerminalKind)        // token missing/expired, shape drift
    case inactive(reason: String)                            // #126: abandoned provider -- muted .info, no red banner
}
struct ProviderResult { let usage: ProviderUsage; let outcome: ProviderOutcome }
```

- The **401 self-heal stays internal** to the provider (reread Keychain + retry once iff the token
  changed). Only the *final* result is classified. A genuine post-retry 401 => `.terminal("Claude
  token expired - re-login")`; expired-on-disk => `.terminal("Claude token stale - open Claude Code to
  refresh")`. Reasons are preserved distinctly, not collapsed.
- Response-shape drift (HTTP 200, normalizer returns nil) => `.terminal("Claude usage: unexpected
  response shape")` so a schema change stays visible.
- **Abandonment demotion (#126).** A `.terminal` carrying `kind: .staleToken(expiredFor:)` past 48 h with no statusline POST inside that window is remapped by AppDelegate to `.inactive("Claude inactive")` (a muted `.info` note, not the red banner) -- an abandoned Claude Code stops nagging while a same-day expiry or a recently-active statusline keeps the actionable `.error`. A `kind: .missingCredential` is never demoted (no durable first-seen timestamp; indistinguishable from a never-logged-in user), so "run claude login" stays actionable. `TerminalKind` is `.missingCredential | .staleToken(expiredFor:) | .other`.
- 429 / 5xx / network error => `.transient(retryAfter:, reason:)`. The `reason` carries the localized
  failure text (e.g. "Claude usage unavailable (HTTP 429)") so the menubar still has an actionable
  string when last-good is *expired* and we fall back to an error.
- `Retry-After` parsing supports both delay-seconds and HTTP-date. Two distinct caps (see
  Configuration): a server-directed `Retry-After` is honored up to `retryAfterSanityCap` (60 min) — NOT
  clamped to the 15 min exponential cap, so an explicit long cooldown is respected; only absurd /
  far-future / negative values are rejected (clock-skew safety).

### 2. Poller: per-provider backoff + oauth gate

Per-provider state confined to the existing serial `beacon.usage` queue:

```swift
struct ProviderPollState { var consecutiveFails: Int; var backoffUntil: Date? }
```

- All mutation happens in `group.notify(queue: queue)` (NOT inside provider completion closures), to
  preserve the serial-queue confinement.
- `shouldPollClaude` is extended: poll oauth only when `statusline-stale AND now >= backoffUntil`. This
  is goal 3 — a 429 storm backs the oauth endpoint off `45s -> 90 -> 180 ...` up to the cap.
- On `.live`: `consecutiveFails = 0`, clear `backoffUntil`. On `.transient`: `consecutiveFails += 1`,
  `backoffUntil = now + backoff(...)`. On `.terminal`: counters unchanged (terminal is not a rate-limit
  signal; statusline-fresh masking must not reset oauth backoff blindly).
- Codex keeps its 45s cadence (it is not the 429 offender) but flows through the same outcome model, so
  it gets last-good retention for free via the reducer.
- **The poller's own `lastClaudeResult` retention is removed** (UsagePoller.swift:40,85,100). Retention
  now lives solely in the reducer; replaying a cached poller result would create double-retention and
  re-emit stale terminal/transient outcomes. When the oauth poll is skipped (statusline fresh or
  backed off), the poller emits **no Claude event** (`claude: nil`), which the reducer treats as "no new
  observation" — distinct from a fresh failure.
- `onUpdate` forwards per-provider, optional `ProviderResult?` (Claude may be `nil` = skipped; Codex
  always present), not a flattened `[String]`, so the reducer in `AppDelegate` sees outcomes by source.

### 3. Pure reducer (the heart) — BeaconHubKit

```swift
struct ProviderRetention: Equatable {
    var lastGood: ProviderUsage?     // last LIVE value (from either source)
    var lastGoodAt: Date?
}
struct ProviderDisplay: Equatable {
    var usage: ProviderUsage         // value to show/send, with .stale set appropriately
    var note: UsageNote?             // nil, or a menubar note (severity + text)
}
// Pure. No Date()/RNG inside — clock and config are inputs.
func reduceProvider(
    prior: ProviderRetention,
    outcome: ProviderOutcome,
    usage: ProviderUsage,
    now: Date,
    maxStale: TimeInterval
) -> (next: ProviderRetention, display: ProviderDisplay)
```

Behavior table:

| Input outcome | last-good | display.usage | display.note |
|---|---|---|---|
| `.live` | store `usage` @ `now` | `usage` (`stale=false`) | nil |
| `.transient`, last-good `staleness == .fresh` | keep | `lastGood` with `stale=true` | `.info("Claude rate-limited - last value HH:MM")` |
| `.transient`, last-good `.expired` or none | keep | `.unavailable` | `.error(reason or generic)` |
| `.terminal` | **cleared** (credential-identity safety) | `.unavailable` | `.error(reason)` |
| `.inactive` | **cleared** (identity safety) | `.unavailable` | `.info(reason)` -- muted, #126 |

**Staleness is per-window, not per-provider.** `h5` and `d7` reset independently, so expiring the
whole provider on `h5.reset` would blank a still-valid 7-day value when only the 5-hour window rolled
over. The reducer evaluates each window:

```
windowExpired(lastGoodAt, now, maxStale, window.reset) -> Bool
  expired when  lastGoodAt == nil  OR  now - lastGoodAt > maxStale  OR  now >= window.reset
```

On `.transient`, each window is kept (served stale) while not expired, or set to `pct = nil` (`--`)
individually once expired. The **wire `stale` flag stays per-provider** (the chosen contract) and is
`true` iff *any* window is currently served stale. Result: `d7` can show a dimmed retained value while
`h5` shows `--` after its window reset.

Statusline live values are fed to the SAME reducer as `.live` events (via the source-precedence rule
above), so they update `lastGood` uniformly; the statusline-freshness gate only governs whether the
oauth endpoint is polled, never the reducer.

### 4. Menubar UX — typed note (beacon-hub)

`MenubarController.setUsage` / `HubPanel` currently take `[String]` and render every entry red. Replace
with a typed note so the soft note is not alarming:

```swift
struct UsageNote: Equatable { enum Severity { case error, info }; let severity: Severity; let text: String }
```

- `.info` => muted/secondary styling (e.g. "Claude rate-limited - last value 12:35 PM"). The timestamp
  is `lastGoodAt` (fixed), not wall-clock, so the note text does not churn every minute.
- `.error` => the existing red banner (terminal failures, expired last-good).

**Two equality channels.** Today `rebuildUsage` gates the menubar update and the BLE frame together
(`merged != usage || errors != lastUsageErrors`). Split them: the **BLE frame is sent only on `Usage`
change** (incl. the per-provider `stale` bool), while a **menubar-note-only change updates the menubar
but does NOT send a frame**. A changing `.info` note must not generate BLE traffic — the 30s heartbeat
already covers any genuinely missed frame.

### 5. Frame + firmware — full per-provider stale, end-to-end

The usage value is shown dimmed on the device when stale. The firmware usage record has a **single
shared `hdr`** for both providers and `ds_set_usage` **force-sets `ST_LIVE`**, so the existing record
state cannot express per-provider staleness. A dedicated per-provider field is added.

**Frame (additive `v:1`, CONTRACT.md §A):** per-provider flag, emitted only when true (mirrors
`qlen`/`loc`):

```json
"claude":{"stale":true,"h5":{"pct":24,"reset":...},"d7":{"pct":24,"reset":...}}
```

**Swift (`Protocol.swift`):** `ProviderUsage.stale: Bool?` — MUST be `nil` on live (never `.some(false)`):
synthesized `Codable` encodes `"stale":false` for `false` but omits `nil`, so the reducer sets `nil`
when live and `true` when stale. Adding the field makes stale-transitions compare unequal under the
synthesized `Equatable`, which is required so `rebuildUsage`'s equality skip (#59, `AppDelegate:315`)
and the statusline dedup do not suppress a live<->stale transition. `EqualityGateTests` is updated to
pin the new field, including an explicit assertion that live encodes with NO `stale` key.

**Firmware:**

- `records.h`: add `bool stale;` to `usage_provider_t` (per-provider; the shared `hdr` keeps meaning
  hub-link state, e.g. `ST_HUB_OFFLINE`).
- `hub_proto.cpp` (`hub_parse_status`): parse `usage.<provider>.stale`, default `false`.
- `datastore.cpp` (`ds_set_usage`): the per-provider `stale` is copied via `s_usage = *r`; the existing
  `hdr.state = ST_LIVE` (hub-link liveness) is unchanged and does not override it.
- **UI views (7 usage themes):** each view derives one `dim = sv_dim(u.hdr.state)` from the shared
  header and applies it to all 4 windows (e.g. `usage_hud.cpp:123`, `usage_led.cpp:129`,
  `usage_oscilloscope.cpp:156`). Change each per-window call site so a provider's windows dim on
  `dim || u.<provider>.stale`: Claude windows use `dim || u.claude.stale`, Codex windows use
  `dim || u.codex.stale`. Placeholder/offline still derive from the shared `hdr` (unchanged). Per-window
  `pct == -1` continues to render `--` (the expired-window case from the reducer).
- `test_hub_proto/test_main.cpp`: add table cases for stale present / absent on each provider.

## Configuration (defaults, adjustable)

| Param | Default | Rationale |
|---|---|---|
| `maxStale` | 30 min | rides out a 429 window; short enough that `--` still means "really gone" |
| backoff base / cap | 45s / 15 min | base = current tick; cap bounds our exponential idle pressure |
| `retryAfterSanityCap` | 60 min | honor a server-directed `Retry-After` up to here; reject only absurd/far-future |
| jitter | +-20% | de-sync repeated 429s across restarts/instances |

`backoff(consecutiveFails, base, cap) -> TimeInterval` is pure and deterministic (exponential, clamped
to the 15 min cap). The final delay composes it with a server-directed cooldown and jitter, in this
order to avoid polling before the server allows:

```
delay = max( retryAfter (clamped to [0, retryAfterSanityCap]),   // floor: never earlier than the server says
             jitter( backoff(fails, base, cap) ) )                // our own exponential, jittered
```

So `Retry-After: 3600` is honored (not clamped to the 15 min exponential cap), and negative jitter can
never schedule a poll before the server-mandated cooldown. Jitter uses an injectable RNG so
`backoff(...)` itself stays deterministic for tests.

## Testing

- **Pure, table-driven (BeaconHubKit):**
  - `backoff(...)` — sequence growth, exponential cap (15 min).
  - delay composition — `Retry-After` floor wins over jittered backoff; negative jitter never schedules
    before the cooldown; `Retry-After: 3600` honored (not clamped to 15 min); absurd/far-future/negative
    `Retry-After` rejected; sanity cap (60 min).
  - `staleness(...)` — per-window: fresh / stale / expired; `maxStale` boundary; window-reset-passed;
    `h5` expired while `d7` still valid.
  - `reduceProvider(...)` — every row of the behavior table, plus: terminal clears last-good; transient
    after a prior live serves stale; per-window expiry (`h5 --` while `d7` dimmed); provider `stale`
    flag = OR of windows served stale; "no event" (skipped oauth) leaves state untouched; statusline
    activity re-affirms `lastGoodAt`; source precedence (a late oauth transient does not downgrade a
    fresh statusline value).
- **Codec parity:** `Protocol.swift` <-> `hub_proto.cpp` round-trip of `stale` true / absent (shared
  CONTRACT.md fixtures), `test_hub_proto` cases.
- **Equality gate:** `EqualityGateTests` updated for the new field; assert live<->stale transitions
  compare unequal (so they are not skipped) AND that a live value encodes with NO `stale` key.
- **Two channels:** assert a note-only change updates the menubar without sending a BLE frame; a `Usage`
  (incl. `stale`) change sends a frame.
- No new URLSession mocking: the reducer extraction means the previously integration-only path
  `(priorState, outcome, now) -> (displayUsage, note, nextBackoff)` is now host-tested.

## Rollout / Compatibility

- Frame change is additive `v:1`; an older device ignores `stale` and shows the (slightly old) number —
  safe. A newer device with an older hub sees no `stale` and renders live — safe.
- No protocol version bump; consistent with prior additive extensions (`qlen`, `loc`).

## Open Questions

None blocking. Defaults above are tunable during implementation review.
