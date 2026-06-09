# Implementation Plan — Issue #13: Re-model Coding Buddy idle/activity to CC hook semantics + per-session attribution

## Scope: Hub-only. No BLE wire change, no firmware change.
Data flow: CC hook payload (session_id/cwd/transcript_path) => ClaudeCodeBridge => BuddyState => StatusFrame.encoded() => BLE => hub_parse_status => buddy_rec_t => 7 buddy views.
The device already renders 3 hub-controlled free-text channels: prompt.hint (BUDDY_HINT_LEN=80), entries[] (BUDDY_ENTRY_LEN=40 x3), and the telemetry line (from scalar running/waiting/tokens/context_pct). Attribution folds into existing hint/entry strings => no new wire fields. Reject the wire-field/firmware option (scope expansion, no width benefit). BuddyState shape unchanged => existing ProtocolTests stay green (wire-compat signal).

## CC hooks reality (from Codex live-docs review) — drives the redesign
- `SessionStart` does NOT support `type:http` in current CC (only `command`/`mcp_tool`), yet the snippet installs it as http (claude-code-settings.snippet.json:20-24). So a SessionStart-based +1 may never fire through the bridge. => Do NOT depend on SessionStart for liveness.
- `SessionEnd` DOES support http and carries session_id, but is exit-cleanup: it will NOT fire on SIGKILL/crash. => Clean-removal path, but needs a TTL backstop.
- `Notification` matcher is `*` => fires for permission_prompt, idle_prompt, auth_success, elicitation_* (not all are "waiting"). => filter/TTL.
- statusline body DOES include session_id (Codex-confirmed current docs); the shim forwards raw stdin (statusline-shim/beacon-statusline:18). => per-session map is viable; update CONTRACT.md.
- PermissionRequest/PreToolUse carry session_id (confirmed in code, ClaudeCodeBridge.swift:224).
- Adding SessionEnd as http needs NO build-app.sh change: the dedupe reduces over snippet.hooks keys, keyed on the inner http url (build-app.sh:60-69).

## Decision 1 — RUNNING = first-seen session map + TTL reaper (NOT a SessionStart counter)
Robust to SessionStart-not-firing AND missed SessionEnd.
- `private var sessions: [String: Date] = [:]` (session_id => lastSeen), queue-confined.
- `touch(_ sid)`: `sessions[sid] = Date()` — called from applySessionHook (any event), handlePermission, handleStatusline (all carry session_id). First-seen handles SessionStart never firing; also re-heartbeats on every event.
- SessionEnd: `sessions.removeValue(forKey: sid)` (clean removal) + prune waitingSessions/sessionStats for sid.
- Reaper: a repeating DispatchSourceTimer on `queue` (~60s) prunes sessions whose lastSeen older than TTL=600s (10 min); recompute running and republish if changed. This bounds the SIGKILL leak to <=10 min instead of forever.
- `buddy.running = sessions.count` (after prune). Stop no longer touches running.
- Documented bound: a truly-idle-but-alive session with no statusline/hook traffic for >10 min may briefly under-count, self-correcting on next activity. (statusLine refreshInterval would give a precise heartbeat — noted as a future option, not done here to avoid changing the user's statusline cadence.)

## Decision 2 — WAITING = real waitingSessions Set (no permanent 0); keep on the wire (dropping it would touch 7 firmware views)
- `private var waitingSessions: Set<String> = []`, queue-confined.
- handlePermission (AFTER the offline/busy early returns at :233-245, so only a genuinely-held prompt marks waiting): insert the prompt's session_id; store `sessionId` on Pending.
- finish (prompt resolved): remove that prompt's sessionId.
- Notification: insert session_id ONLY for waiting-type notifications. Determine the type from the body (confirm field against a live payload; likely a `message`/type marker). If the type is not reliably machine-readable, include all Notifications but rely on the Stop/SessionEnd/TTL clears (a brief false-wait from auth_success self-clears on the next Stop). Document the chosen filter.
- Stop / SessionEnd for a session_id: remove from waitingSessions (turn advanced / ended).
- Reaper also drops waitingSessions entries whose session was reaped from `sessions` (TTL backstop against a stuck wait).
- `buddy.waiting = waitingSessions.count` after each mutation.

## Decision 3 — TOK/CTX = explicit aggregate (not last-writer-wins)
Device line has ONE fixed-width scalar slot x7 views => per-session labeled scalars don't fit and would need wire fields (rejected).
- `sessionStats: [String:(tokens:Int, ctxPct:Int)]` keyed by session_id (from statusline body["session_id"]), replacing the last-writer-wins overwrite at handleStatusline:304-311.
- buddy.tokens = SUM across sessions ("total work in flight"); buddy.contextPct = MAX ("most-pressured context" — sum/avg are meaningless/hiding).
- Prune sessionStats on SessionEnd AND in the TTL reaper (so a dead session stops inflating the sum).
- Update CONTRACT.md statusline section to list session_id (removes the flagged unknown). If a live payload lacks session_id, fall back to scalar last-writer-wins and document; do NOT invent a wire field.

## Decision 4 — Relabel Stop entries + cwd attribution (string-only, in entryLine)
- Stop: "session stopped" => "turn done" (turn finished, session alive).
- Add SessionEnd => "session ended".
- Prefix every entry with cwd basename: "HH:MM beacon turn done". Basename (trailing path component, ~10 char trunc) because BUDDY_ENTRY_LEN=40. Add static cwdTag(from body:) helper, reused by entries + prompt hint.
- ADJACENT BUG (fold in): records.h newest-first contract, device renders entries[0] as prominent slot, but applySessionHook does suffix(4)+[entry] => newest LAST => device shows oldest. Fix: ([entry] + buddy.entries).prefix(3) (newest-first, cap 3 = BUDDY_ENTRIES).

## Decision 5 — Permission prompt attribution (no wire field)
- handlePermission: prefix hint with cwdTag => "[beacon] <commandHint>" (cwd is the disambiguator; front-load it; BUDDY_HINT_LEN=80 has room). Use same cwdTag helper.
- session_id stored on Pending for waiting-set clearing; not shown (cwd basename is the human disambiguator).

## Change list (file:line — executor must re-read actual lines)
ClaudeCodeBridge.swift:
- State (near :52): `sessions: [String:Date]`, `waitingSessions: Set<String>`, `sessionStats: [String:(Int,Int)]`, `reaper: DispatchSourceTimer?`. All mutated ONLY on `queue`.
- Pending (:44-51): add `let sessionId: String`.
- start()/bind: start the reaper timer on `queue` (repeating ~60s) calling a `reap()` that prunes by TTL and republishes.
- router (:214): add `"SessionEnd"` to the applySessionHook case.
- handlePermission (:222-262): read `session_id`; AFTER the offline/busy returns insert into waitingSessions; pass sessionId to Pending; prefix hint with cwdTag; call touch(sid).
- finish (:264-278): remove the resolved prompt's sessionId from waitingSessions; recompute waiting.
- applySessionHook (:282-298): touch(sid) for every event; SessionStart no special count; Stop removes sid from waitingSessions (no running change); Notification inserts waiting (filtered, see D2); SessionEnd removes sid from sessions/waitingSessions/sessionStats; running=sessions.count; entries newest-first `([entry] + buddy.entries).prefix(3)`.
- handleStatusline (:300-321): touch(sid); key sessionStats by session_id; buddy.tokens=sum, buddy.contextPct=max.
- entryLine (:360-370): Stop => "turn done"; add SessionEnd => "session ended"; prefix all with cwdTag.
- add `static func cwdTag(from body:) -> String` (trailing path component of body["cwd"], trunc ~10) near commandHint (:351).
- add `private func reap()` (queue) + `private func touch(_ sid: String?)`.
hub/claude-code-settings.snippet.json: add SessionEnd http hook (matcher "*", timeout 5); update _comment (SessionEnd drives clean session removal; running is first-seen+TTL).
hub/CONTRACT.md: add session_id to the statusline payload section (was draft-omitted).
Tests: BuddyState wire encoding unchanged => existing ProtocolTests stay green (wire-compat signal). ClaudeCodeBridge is in the beacon-hub target with no test target. If a quick test target is cheap, cover running=distinct sessions, waiting clears on Stop, tokens=sum/ctx=max, entries newest-first, cwdTag basename. Otherwise flag as optional and rely on the manual device test. Note: time-based reaper is hard to unit-test without injecting a clock — keep reap() taking an explicit `now` param so it is testable.
NO changes to Protocol.swift, AppDelegate.swift, firmware/*.

## Thread-safety (Codex)
All new state is queue-confined. The reaper MUST be a DispatchSourceTimer scheduled on `queue` (like the cap timer at :248). publishBuddy already hops to main for the callback. Never read/mutate sessions/waitingSessions/sessionStats from AppDelegate's main callbacks.

## Conventions / pushback
- WHY-comments, ASCII-only, => not arrows.
- Rejected wire-field/firmware option and per-session TOK/CTX labels (width).
- Confirm statusline session_id against live payload; no invented wire field if absent.
- Folded adjacent entries newest-first ordering bug.
