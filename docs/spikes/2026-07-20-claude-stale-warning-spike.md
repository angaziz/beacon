# Spike: persistent "Claude token stale" warning after abandoning Claude Code (#126)

Investigated hub behavior when the Keychain Claude credential is expired and the user no longer runs Claude Code. All findings are code-traced; file references are current as of this date. Reviewed by an advisory model; its corrections (statusline bypass scoping, token-missing coverage gap, layering decision for the abandonment predicate) are folded in.

## Answers to the spike questions

### Q2 first (it reframes everything): does the poller keep running with the toggles off?

**No. Polling and credential reads already stop. The bug is that the banner does not clear.**

- `UsagePoller.poll()` filters `p.descriptor.supportsUsage && usageEnabled(id)` before touching `usageSource.fetch` (`UsagePoller.swift:73`). `ClaudeUsageProvider.credential()` is only reached from `fetch()`, so a usage toggle-off stops Keychain reads entirely.
- `ProviderMux.publishUsage()` filters `usageEnabled(id)` (`ProviderMux.swift:177`), so toggle-off removes the CLAUDE card from the merged Usage (menubar cards + BLE frame) immediately via `applyEnabled => pushMenubarUsage`.
- **The gap:** the red banner lives in `AppDelegate.notes[id]`, written only by `reduce()` (`AppDelegate.swift:458`; the only other `notes` access is the read in `pushMenubarUsage`). `pushMenubarUsage()` renders `registrationOrder.compactMap { notes[$0] }` with **no** `usageEnabled` filter (`AppDelegate.swift:464-467`). After toggle-off, the poll path skips the provider (`AppDelegate.swift:404`), so nothing clears the last terminal note.
- **Scoping caveat (statusline bypass):** `onStatuslineActivity()` and `onStatuslineClaude(_:)` call `reduce("claude", .live, ...)` unconditionally, with no `usageEnabled` gate (`AppDelegate.swift:431-448`). So "frozen forever" holds exactly when there is no statusline traffic, which is precisely the abandoned scenario of #126. Conversely, a user who toggles usage OFF but keeps using Claude Code still has the statusline writing hub-side display/note state for a disabled provider (the mux filters it off the wire, so only internal state and the note map churn). Any toggle-silencing fix must gate these two handlers too.

So toggle-off today: tile disappears, credential reads stop, but the "Claude token stale" banner persists until the process exits.

### Q1: demote long-expired token from warning to a quiet "inactive" state?

Feasible and cheap. `ClaudeCredential.expiresAt` is parsed from the Keychain blob (`ProviderCredentials.swift:34-36`), so the expired-on-disk branch (`UsagePoller.swift:132-135`) can compute `now - expiresAt` at zero extra cost. The reducer already carries two severities (`UsageNote.Severity`, `UsageReducer.swift:16-23`); the terminal case just needs a severity input instead of hardcoding `.error` (`UsageReducer.swift:57-60`).

Threshold: expired >= 48 h => demote. Claude access tokens are short-lived (hours in practice; calibrate the constant against observed `expiresAt` deltas during implementation, this spike did not measure it), so days-expired means the CLI has not run at all, while a same-day expiry keeps the actionable message.

Caveats:
- Demotion alone still shows a "--" CLAUDE tile (terminal clears last-good, `UsageReducer.swift:57-60`). Q1 without Q4 only quiets the menubar.
- The demoted `.info` note is subject to the same frozen-note mechanics; it must ride the same toggle filter as the `.error` banner.
- **Coverage gap:** "Claude token missing - run claude login" (`UsagePoller.swift:127`) has no credential, hence no `expiresAt`; an expiry-age predicate can never demote it. An abandoned user who logged out or lost the Keychain item keeps that banner forever. The abandonment predicate must treat missing-credential as demotable too (see Q4).

### Q3: explicit "disconnect Claude" affordance?

Skip it, conditional on the Q2 fix landing. There is no hub-persisted credential to clear beyond the in-memory `cachedCredential` (dropped on restart, never re-read while usage is off), and the Keychain item belongs to Claude Code, not the hub. "Both toggles off" is the disconnect affordance, but that argument only becomes true once the statusline handlers are gated (bypass above) - today the toggles do not fully silence the provider. Whether the buddy toggle gates the statusline shim ingest was not traced in this spike; the usage gate on the AppDelegate handlers is sufficient regardless.

### Q4: auto-detect abandonment?

The signals exist and are already in `AppDelegate`:

- `statuslineClaudeAt` (last ClaudeCodeBridge rate_limits POST) drives statusline-vs-oauth precedence (`AppDelegate.swift:406-407`). Note: in-memory only, nil after every launch, so post-restart the predicate degenerates to the credential-side condition alone. That still serves the abandoned case correctly; persisting it is optional hardening.
- Token expiry duration from `expiresAt` (above), plus the missing-credential terminal.

Proposed rule, evaluated where both signals live (see layering below):

```
inactive := (credentialMissing || now - expiresAt > 48h)
         && (statuslineClaudeAt == nil || now - statuslineClaudeAt > 48h)
```

(The `nil` disjunct covers both never-ran-this-launch and post-restart; it intentionally errs toward inactive because the credential condition already proves days of CLI silence.)

Inactive => no `.error` banner; a muted `.info` "Claude inactive" or nothing. Keep the tile as "--" rather than hiding it while usage is enabled: hiding changes the wire Usage shape for an enabled provider, which is a device/contract question this spike did not open. Any statusline event or a non-expired credential flips back to normal instantly, so a returning user self-heals with zero interaction.

**Layering decision:** the terminal classification happens inside `ClaudeUsageProvider.fetch()`, which has `expiresAt` but not `statuslineClaudeAt` (AppDelegate-owned). Recommended split: the provider reports a structured terminal kind (e.g. `.terminal(reason:, kind: .staleToken(expiredFor:))` or equivalent), and AppDelegate combines it with statusline liveness before calling `reduce` with the final severity. The pure predicate (`inactive(expiredFor:missing:statuslineAge:)`) lands in `UsagePollDecision` where it is host-testable. Do NOT push statusline liveness down into the provider (coupling) or fork the taxonomy across layers ad hoc.

## Recommendation for the follow-up fix

Ship in two steps. Step 1 fixes the toggle path; step 2 fixes the default-on path. Both are required by the acceptance criteria: bullet 1 ("no persistent warning" for a user who left) covers the default-on user who never opens Settings, which only step 2 addresses.

1. **Toggle-off silences the provider** (small, unambiguous):
   - Filter `pushMenubarUsage()` notes by `usageEnabled(id)` AND clear `notes[id]` + provider retention in `applyEnabled` on usage-off. Both are needed: the filter alone would let the pre-toggle stale note resurface on re-enable before the first poll; the clear alone leaves the statusline hole below.
   - Gate `onStatuslineActivity` / `onStatuslineClaude` on `usageEnabled("claude")` so a disabled provider's state stops churning.
   - Re-enable UX: `mux.setEnabled` republishes its cached `usageByID[id]` (`ProviderMux.swift:81, 176-183`); clearing `notes[id]`/`retentions[id]` does NOT reset that cache. So the tile on re-enable shows the last mux display value until the next poll: `--` in the #126 stale/terminal case (the terminal path already fed the mux `.unavailable`), or the last live value for a provider that was live when toggled off (acceptable - a plausible last value, not a blank). Do NOT force a `--` flicker by clearing the mux cache; if a hard reset is ever wanted, clear `displays[id]` and call `mux.provider(id, didUpdateUsage: .unavailable)` on usage-off, but that is not needed for #126.
   - Testability: extract the note-assembly (order + enabled filter) as a pure helper into `BeaconHubKit` and table-test it there, per the repo's host-tested-logic pattern.
2. **Inactive demotion** (Q1+Q4 combined): structured terminal kind from the provider, abandonment predicate in `UsagePollDecision` (pure, covers stale-token AND missing-credential), severity threaded through `UsageReducer.reduceProvider`. Update the terminal-reason taxonomy in `specs/2026-06-25-hub-usage-reliability-design.md`.

Acceptance bullet 3 (fresh-but-expired token while Claude Code is in active use) is preserved twice over: the expiry-age threshold and the statusline-liveness condition each independently keep the actionable `.error` message.

Skip Q3 (no new affordance).

## Evidence index

| Claim | Location |
|---|---|
| Toggle-off stops fetch + credential reads | `UsagePoller.swift:73`; `credential()` only called from `fetch()` (`UsagePoller.swift:107-137`) |
| Toggle-off removes menubar card + wire entry | `ProviderMux.swift:86-88, 176-183` |
| Banner not filtered by toggle, never cleared by poll path | `AppDelegate.swift:404, 458, 464-467` |
| Statusline handlers bypass the usage toggle | `AppDelegate.swift:431-448` |
| Terminal clears last-good => "--" tile; hardcoded `.error` | `UsageReducer.swift:57-60` |
| Two note severities exist | `UsageReducer.swift:16-23` (`UsageNote.Severity`) |
| expiresAt available; missing credential => distinct terminal | `ProviderCredentials.swift:11-36`; `UsagePoller.swift:125-135` |
| Statusline liveness signal exists, in-memory only | `AppDelegate.swift:406-407, 431-432` |
