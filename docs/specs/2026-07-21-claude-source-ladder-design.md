# Claude usage source ladder: delegated token refresh with direct-refresh fallback (#132)

Extends the usage-reliability design (2026-06-25) and the #126 quiet floor. Mechanism verified against CodexBar's source (steipete/CodexBar @ cc8da27), which solves the identical problem with an ownership-aware ladder.

## Problem

An expired-on-disk Claude OAuth token made the hub give up: terminal `.staleToken`, `--` tiles, and (post-#126) a muted "Claude inactive" note. But the credential blob in the Keychain carries a `refreshToken` (+ `refreshTokenExpiresAt`), so the token is refreshable for days after the last Claude Code run. Giving up was a policy choice, not a technical limit.

## Ownership rule (the load-bearing invariant)

Anthropic refresh tokens are single-use and rotating: whoever redeems one invalidates every other holder's copy.

- `claude` CLI installed => the CLI owns the refresh lifecycle. The hub NEVER redeems the refresh token itself (that would log the user out of Claude Code); it makes the CLI do the refresh (delegated).
- CLI binary absent => nobody else can use the token. The hub may redeem it directly, persisting rotations to its OWN Keychain item, never the CLI's.

This is exactly CodexBar's split; its code comment: "Claude Code rotates refresh tokens; when its storage exists, it owns the refresh lifecycle."

## The ladder

Order of preference for the Claude usage source; each rung fills the failure mode of the one above.

| Rung | Source | Fires when | Status |
|---|---|---|---|
| 1 | Statusline shim (rate_limits POST) | Claude Code in active use | exists (#93) |
| 2 | OAuth poll `api.anthropic.com/api/oauth/usage` | valid token on disk | exists (#64/#108) |
| 3 | Delegated refresh: spawn `claude` PTY, `/status`, confirm Keychain changed, re-read, poll | token expired AND CLI installed AND refresh token alive | NEW |
| 4 | Direct refresh: POST `platform.claude.com/v1/oauth/token` (grant_type=refresh_token, public client_id `9d1c250a-e61b-44d9-88ed-5944d1962f5e`), cache rotations in Beacon's own item | token expired AND CLI absent AND refresh token alive | NEW |
| 5 | Actionable banner "Claude session expired - run claude login" | refresh token itself expired | NEW reason, existing mechanics |
| 6 | Quiet floor: muted "Claude inactive" | terminal `.staleToken` + no statusline activity within 48 h | exists (#126) |

Rung 5 keeps `kind: .staleToken` deliberately: an abandoned user with a dead refresh token must still demote to rung 6 via the existing statusline-age predicate.

## Layering (matches the transport/policy split)

- `BeaconHubKit` (pure, host-tested): `ClaudeRefreshDecision.path(cliAvailable:refreshTokenAlive:)` => `.delegated | .direct | .none`; `shouldAttempt(secondsSinceLastAttempt:cooldown:)`; `parseRefreshResponse(_:now:)` isolating the OAuth response shape. `ClaudeCredential` gains `refreshToken`/`refreshTokenExpiresAt` + `refreshTokenAlive(at:)`.
- `beacon-hub`: `ClaudeTokenRefresher` (PTY spawn, Keychain confirm, direct POST, Beacon-owned cache) wired into `ClaudeUsageProvider.fetch()`'s expired branch. AppDelegate and the #126 demotion are untouched; the ladder sits above them.

## Guardrails (each plugs a real hole, all from CodexBar's field experience)

- Cooldown between refresh attempts: 300 s after success, 20 s after failure. A wedged CLI must not be respawned every 45 s poll tick.
- Success = the Keychain credential actually CHANGED (different accessToken or no longer expired), never mere CLI exit 0. Otherwise a failed refresh coasts through a long cooldown with a dead token.
- ONE Keychain re-read per attempt, after the CLI exits. Each SecItemCopyMatching can prompt unless the user chose Always Allow; polling in a loop would prompt-storm.
- Probe session artifact cleanup: the spawned CLI runs in a throwaway temp cwd, and `~/.claude/projects/<munged-cwd>` is deleted afterward. Beacon's own Coding Buddy lists sessions (#111); leaking probe sessions would pollute the device UI with junk.
- Direct refresh is hard-gated on CLI absence by `path()`; no caller can bypass the ownership rule.
- The rotated refresh token is preserved: if the token endpoint omits `refresh_token` in the response, the current one is carried forward (dropping it would strand the user).

## Failure modes

| Scenario | Behavior |
|---|---|
| Refresh token expires (hard ceiling, ~days after last CLI use) | rung 5 banner; demotes to rung 6 when abandoned |
| CLI uninstalled mid-life | rung 4 takes over using the last known credential (CLI item or Beacon cache) |
| `/usage` endpoint 429s after successful refresh | existing transient/backoff machinery (#108), unchanged |
| CLI wedges or PTY times out | failure cooldown (20 s), retry next eligible tick, ladder falls through to rung 5/6 meanwhile |
| Keychain not Always Allow | at most one extra prompt per attempt, attempts bounded by cooldown |

## Non-goals

- No hub-side OAuth login flow (authorization code/PKCE). If no credential exists anywhere, the answer stays "run claude login".
- No TUI scraping of `claude /usage` output (CodexBar's third path): fragile parser, and our oauth/usage JSON path already covers the data need once the token is fresh.
- No BLE/device contract change; CONTRACT.md untouched.
