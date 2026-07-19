# hub/CONTRACT.md — recorded fixtures (P2-0)

> The shared fixture set so the device codec (`firmware/.../core/hub_proto.cpp` + `test_hub_proto`)
> and the hub (`BeaconHubKit` + its tests) are tested against the **same** payloads (`tech.md` §7.3).
>
> **Status:** the **device-facing frame + commands (§A/§B) are FROZEN** in `tech.md` §7.1 and final.
> The **upstream shapes (§C) are RECORDED** — §C.1/§C.2 are real, token-redacted captures from the
> owner's Mac (2026-06-11) and §C.3/§C.4 are confirmed against the Claude Code v2.1.x docs. The P2-0
> draft guesses matched the live shapes on every field the normalizer reads. Nothing here may contain
> a real token.

## A. Hub -> device status frame (FROZEN, `tech.md` §7.1)

Newline-delimited JSON, `"v":1`. `usage` and `buddy` are independently optional (send what changed;
the device keeps an absent block's last values). A null/omitted window `pct` => unavailable ("--").

```json
{"v":1,"usage":{"providers":[
  {"id":"claude","label":"CLAUDE","h5":{"pct":24,"reset":1717600000},"d7":{"pct":24,"reset":1717800000},"stale":true},
  {"id":"codex","label":"CODEX","h5":{"pct":1,"reset":1717590000},"d7":{"pct":29,"reset":1717800000}}]},
 "buddy":{"running":2,"waiting":1,"tokens":184502,"context_pct":42,
          "entries":["10:42 git push","10:41 yarn test"],
          "prompt":{"id":"p07","agent":"claude","tool":"Bash","hint":"rm -rf /tmp/build","qlen":2}}}
```
- Absent `buddy.prompt` => idle. `pct` is an integer 0..100 or JSON null (device reads null/absent as -1).
- The device codec (`hub_parse_status`) + `test_hub_proto` assert exactly this shape.
- `usage.providers` (**BREAKING**, design 2026-07-19, clean cutover from the old fixed
  `usage.{claude,codex}` slots) -- 0..4 entries, one per usage-toggle-ON provider, in hub display
  order. Each entry: `id` (stable lowercase ascii, <=12 chars), `label` (display string, <=10 chars,
  uppercase preferred), `h5`/`d7` windows, and optional `stale`. The device renders provider labels
  from the record instead of hardcoding "claude"/"codex"; usage themes show the first 2.
- `usage.providers[].stale` (additive `v:1` ext, issue #108) -- `true` => the windows carry
  last-known-good the hub held through a transient failure (e.g. Claude oauth 429); the device dims that
  provider's windows. Emitted ONLY when `true` (absent/`false` => live), like `qlen`. Per-provider,
  independent.
- **Migration:** this is a breaking change to the `usage` block only. Old firmware fails to parse the
  new `providers` array and shows usage unavailable ("--"); flash matching firmware (the web flasher
  makes this trivial). `buddy`/`loc`/`sessions` are unaffected.
- `buddy.prompt.qlen` (additive `v:1` ext, issue #98) -- total pending prompts incl. the shown front.
  Omitted or `<=1` => a single prompt (no `(1 of N)` badge). The device always shows the front;
  position is implicitly 1, so there is no `qpos`.

Optional `loc` block (additive `v:1` extension, issue #54). The hub sources lat/lon + `name` from
macOS CoreLocation/CLGeocoder and `tz` from `TimeZone.current`. Independently optional, like
`usage`/`buddy`; parsed by `hub_parse_loc`. Sent ONLY in the (re)connect full frame and in a
loc-only frame on meaningful (> ~0.01 deg) change — **never** on the 30s heartbeat.

```json
{"v":1,"loc":{"lat":-6.91,"lon":107.61,"tz":"Asia/Jakarta","name":"Sukajadi, Bandung"}}
```
- Device precedence: hub `loc` > cached NVS > IP geolocation; a hub fix is never overwritten by IP.
- Permission denied / no fix => the hub omits `loc` and the device keeps its IP-based place name.

Optional `sessions` block (additive `v:1` extension, issue #110). A **standalone** frame — NOT embedded
in `buddy`: the combined `usage`+`buddy`+`loc` status frame already nears `HUB_FRAME_MAX` (1024 B), and a
session array would push it over (silently dropped). A separate frame keeps the budget independent and
lets old firmware ignore it while still reading the unchanged `buddy`/`entries` block. The hub emits it
on any session-state change and on (re)connect; parsed by `hub_parse_sessions` into `buddy_rec_t.sessions[]`.

```json
{"v":1,"sessions":[{"id":"s3","agent":"claude","label":"beacon · fix/109","state":"attention","ts":1719400000},
                   {"id":"s1","agent":"codex","label":"api · main","state":"working","ts":1719399860}]}
```
- Newest-first (hub-sorted by last update). **Frozen caps** (worst case asserted < 1024 B): `sessions`
  length ≤ **5**; `id` ≤ **6** chars (`s` + monotonic counter, wraps mod 100000); `label` ≤ **28** chars
  (`folder · branch`; default branches `main`/`master` dropped); `state` ∈ {`working`, `waiting`,
  `waiting_queued`, `attention`, `question`, `idle`}; `ts` epoch seconds. Unknown `state` => `working`
  (device). `question` = the session is waiting on the user's input (from the CC `Notification` hook);
  the device surfaces it as a "tap to answer on Mac" takeover (priority: permission prompt > question >
  list). The device renders up to 4 rows on the `claude` screen.
- `agent` (additive `v:1` ext, design 2026-07-19) = the owning provider id. Optional on the wire
  (omitted when nil), always emitted by the new hub; the device stores it (cap **12** chars) and may
  ignore it for now. Also carried on `buddy.prompt.agent` (same semantics). `buddy.running`/`waiting`
  count across all buddy-enabled providers; `tokens`/`context_pct` come from whichever provider reports
  metrics (0 otherwise). Session `sN` + prompt `pN` ids stay hub-minted, globally unique across
  providers; device `permission`/`open` commands are unchanged and the hub routes them to the owner.
- Migration: `buddy.entries` stays emitted/legal for back-compat; new firmware reads `sessions` and
  ignores `entries`, old firmware ignores the unknown `sessions` frame. No version bump.

## B. Device -> hub commands + hub acks (FROZEN, `tech.md` §7.1)

```json
{"v":1,"cmd":"permission","id":"p07","decision":"approve"}   // or "deny"
{"v":1,"ack":"p07","ok":true}                                // decision applied
{"v":1,"ack":"p07","ok":false}                               // decision did NOT apply (late/superseded)
{"v":1,"err":"unknown_prompt_id","id":"p07"}                 // id the hub never minted
{"v":1,"cmd":"open","id":"s3"}                               // device tap -> focus this session (issue #110, Phase 2)
{"v":1,"ack":"s3","ok":true}                                 // focus attempted (best-effort tier succeeded)
{"v":1,"err":"unknown_session","id":"s3"}                    // session id the hub never minted / already reaped
```
- `id` echoes the hub-minted short id (see §D). The hub maps it back to the real hook request id.
- `ok:false` = the device decided but the hub had already resolved the prompt (e.g. the ~590 s fail-closed
  cap fired first, or it was superseded). The device must surface this, not treat it as success.
- `open` (additive `v:1`, Phase 2): the device sends it when a session row is tapped; the hub resolves the
  `s<id>` to its captured host context and focuses that terminal/editor (tiered best-effort). `ok:false` =>
  could not focus (e.g. Automation permission denied / app gone); `unknown_session` => stale/never-minted id.

## B2. Hub -> device ticker config + device -> hub config ack (FROZEN, issue #92, design `docs/specs/2026-06-17-hub-ticker-config-design.md` §2)

The hub is the source of truth for the device's market-ticker list. It pushes a **full-snapshot
replace** as a chunked `config` frame; the device persists it (NVS), live-applies it, and acks once per
completed snapshot. Frozen blocks (status/buddy/loc/permission) are untouched. Mirror of
`firmware/src/core/hub_proto.cpp` (`hub_parse_config_chunk` / `hub_config_accum_step` /
`hub_build_config_ack`) and `BeaconHubKit/TickerConfig.swift`.

### Hub -> device: config frame (chunked)

```json
{"v":1,"config":{"rev":7,"part":0,"parts":2,"tickers":[
  {"id":"ygspc","src":"yahoo","sym":"%5EGSPC","name":"S&P 500","kind":"index","cadence":300,"stale":600,"basis":"prev_close"}]}}
{"v":1,"config":{"rev":7,"part":1,"parts":2,"tickers":[
  {"id":"bbtcusdt","src":"binance","sym":"BTCUSDT","name":"BTC","kind":"crypto","cadence":60,"stale":600,"basis":"24h"}]}}
```

- `rev` — hub's monotonic revision (uint32). Echoed in the ack; correlates ack to push.
- `part` / `parts` — 0-based chunk index and total. Rows concatenated across parts in `part` order ==
  display order (full replace).
- **Chunking rule:** each serialized newline-terminated line is `<= ~900 B` (margin under firmware
  `HUB_FRAME_MAX`=1024). A row is **never split** across chunks. The hub packs as many whole rows per
  chunk as fit; an empty list is never pushed (the device rejects an empty assembled snapshot). Encoder:
  `JSONEncoder(.sortedKeys)` + a trailing `0x0A`, matching the status-frame framing.
- **Row keys (exact):** `id` (<=15 chars, stable, deterministic from `(src,sym)`, invariant under
  reorder/removal), `src` (`binance`|`yahoo`), `sym` (Yahoo percent-encoded **once** for the URL path,
  e.g. `^GSPC` => `%5EGSPC`; Binance raw), `name`, `kind` (`fx`|`crypto`|`index`|`etf`), `cadence` (int
  seconds), `stale` (int seconds), `basis` (`prev_close`|`24h`).

### Device -> hub: config ack (one per completed `rev`)

```json
{"v":1,"cmd":"config_ack","rev":7,"ok":true,"count":8}
{"v":1,"cmd":"config_ack","rev":7,"ok":false,"err":"too_many_tickers"}
```

Uses the device->hub `cmd` channel (parsed by `DeviceCommand.configAck`); it does **not** overload the
prompt-id `ack`. On `ok:true`, `count` = applied ticker count. On reject the device keeps its current
list (fail closed) and reports the first `err`:

| `err` | meaning |
|---|---|
| `too_many_tickers` | assembled count > MAX_TICKERS (16) |
| `empty` | assembled count == 0 |
| `bad_source` | `src` not `binance`/`yahoo` |
| `bad_kind` | `kind` not `fx`/`crypto`/`index`/`etf` |
| `bad_basis` | `basis` not `prev_close`/`24h` |
| `bad_chunking` | out-of-order / duplicate / gap / `rev` mismatch / window timeout |
| `nvs_write_failed` | persisted blob write failed; active table untouched |
| `malformed` | invalid JSON, bad `v`, over-length/empty `id`/`sym`/`name`, bad `part`/`parts` |

**Field caps (UTF-8 bytes, enforced by the device — the hub MUST emit within these or the row is `malformed`):** `id` ≤15, `sym` ≤23, `name` ≤23. The hub clamps `name` to fit (display-only) and drops a candidate whose `sym` exceeds the cap.

## B3. Device -> hub ticker report (additive, issue #105, design `docs/specs/2026-06-19-device-ticker-report-design.md`)

So a fresh (never-configured) hub adopts the list the device already holds, the device emits a one-way
`report` on the device->hub `cmd` channel, **once per connection** after the first inbound hub frame.
Full rows, chunked exactly like §B2 `config` (same row schema/caps), but the envelope is flat `cmd`
fields (not a nested `config` object). The hub adopts only when its store is pristine
(`rev == 0 && rows.isEmpty`); otherwise it ignores the report and stays the source of truth. Mirror of
`firmware/.../hub_proto.cpp` (`hub_report_plan` / `hub_build_report_frame`) and
`BeaconHubKit/Protocol.swift` (`DeviceCommand.report`) + `ReportAssembler`.

```json
{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":2,"tickers":[
  {"id":"ygspc","src":"yahoo","sym":"%5EGSPC","name":"S&P 500","kind":"index","cadence":300,"stale":600,"basis":"prev_close"}]}
{"v":1,"cmd":"report","what":"tickers","rev":0,"part":1,"parts":2,"tickers":[
  {"id":"bbtcusdt","src":"binance","sym":"BTCUSDT","name":"BTC","kind":"crypto","cadence":60,"stale":600,"basis":"24h"}]}
```

- `cmd` = `"report"`; `what` = `"tickers"` (namespaces the verb; the hub ignores any other `what`).
- `rev` is **always `0`** -- the device does not persist the hub's rev, and the hub never uses the
  reported value (it adopts its own pristine `rev 0 -> 1`). Carried for structural symmetry + chunk
  continuity only.
- `part` / `parts`, chunking budget, and row keys/caps are **identical to §B2** (rows concatenated in
  `part` order == display order; line <= ~900 B; row never split; <= 16 rows; trailing `0x0A`).
- **No ack.** One-way and informational. An older hub that does not know `cmd:"report"` drops it
  (`DeviceCommand.parse` returns `nil` on an unknown `cmd`).

## C. Upstream shapes (RECORDED — real token-redacted captures, 2026-06-11)

### C.1 Claude usage — statusline `rate_limits` (PRIMARY); `oauth/usage` (FALLBACK)
**Live Claude usage comes from the statusline `rate_limits` (§C.4)** — first-party, no token. The
`oauth/usage` fallback is **intermittent**: it has returned 429 (Anthropic's subscription-limits
change) but answered 200 at this capture, so keep it best-effort. Fallback endpoint headers:
`Authorization: Bearer <tok>`, `anthropic-beta: oauth-2025-04-20`, `User-Agent`. Token: Keychain
`Claude Code-credentials` (access token at `claudeAiOauth.accessToken`; refresh/expiry also present).
Normalizes to `usage.claude` (`utilization`->`pct`, ISO `resets_at`->epoch). `resets_at` carries
microsecond precision + a `+00:00` offset; extra windows (`seven_day_sonnet`, `extra_usage`, ...) are
ignored. Real redacted capture:
```json
{"five_hour":{"utilization":8.0,"resets_at":"2026-06-11T03:30:00.110763+00:00"},
 "seven_day":{"utilization":32.0,"resets_at":"2026-06-15T00:00:01.110782+00:00"},
 "seven_day_sonnet":{"utilization":2.0,"resets_at":"2026-06-15T00:00:01.110788+00:00"},
 "extra_usage":{"is_enabled":false,"utilization":null,"disabled_reason":"org_level_disabled_until"}}
```

### C.2 Codex usage — `GET chatgpt.com/backend-api/wham/usage`
Headers: `Authorization: Bearer <tok>`, `chatgpt-account-id: <id>`. Token: `~/.codex/auth.json`
(`tokens.access_token`, `tokens.account_id`). The draft P2-0 guess matched the live shape: the path is
`rate_limit.{primary_window,secondary_window}.{used_percent,reset_at}` with `reset_at` in epoch
seconds. `used_percent` arrives as an Int here (the normalizer also accepts Double/String); extra
fields (`allowed`, `limit_reached`, `limit_window_seconds`, `reset_after_seconds`, and the top-level
`credits`/`plan_type`/...) are ignored. Normalizes to `usage.codex`. Real redacted capture:
```json
{"plan_type":"plus",
 "rate_limit":{"allowed":false,"limit_reached":true,
   "primary_window":{"used_percent":1,"limit_window_seconds":18000,"reset_after_seconds":18000,"reset_at":1781151661},
   "secondary_window":{"used_percent":100,"limit_window_seconds":604800,"reset_after_seconds":15234,"reset_at":1781148895}},
 "credits":{"has_credits":false,"unlimited":false,"balance":"0"}}
```
Local fallback (D1, **unimplemented**): the `codex` CLI also records usage token-free in
`~/.codex/sessions/**/rollout-*.jsonl` under `rate_limits.{primary,secondary}` — note the keys differ
from the endpoint's `*_window` (they are null until a window is hit), so a future wiring needs its own
normalizer, not `UsageNormalizer.codex`.

### C.3 Claude Code permission hook (`PermissionRequest`, primary; `PreToolUse`, back-compat) — CONFIRMED (CC v2.1.x docs)
Claude Code supports native **`"type":"http"`** hooks (no curl forwarder needed). `PreToolUse` and
`PermissionRequest` are **distinct** events, and **`PermissionRequest` is the one Beacon hooks**:
`PreToolUse` fires on **every** tool call, so holding it open ~590 s would block routine `Read`/`Grep`
(and a narrow matcher like `Bash` misses `Write`/`Edit`); `PermissionRequest` fires **only when a tool
actually needs permission**, so `matcher:"*"` is safe and covers all tools. The bridge still accepts
`PreToolUse` for back-compat. Request body (same fields both events):
```json
{"session_id":"abc","tool_use_id":"toolu_01","hook_event_name":"PermissionRequest",
 "tool_name":"Write","tool_input":{"file_path":"/x","command":"...","description":"..."}}
```
Hint = `tool_input.command` (Bash) | `file_path` | `description`. Correlation id = `tool_use_id`/
`session_id` (the hub mints its own short BLE id and maps it).

**Response shape DIFFERS by event** (`HookResponse.permission`, `Protocol.swift`) — emitting the wrong
one silently fails to gate the tool:
```json
// PermissionRequest (primary): decision.behavior; message only on deny; updatedInput NOT required for allow
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny","message":"Denied on Beacon device"}}}
// PreToolUse (back-compat): permissionDecision in {allow,deny,ask}; precedence deny>ask>allow
{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow","permissionDecisionReason":"Approved on Beacon device"}}
// AskUserQuestion (a question, not yes/no): hub never holds it -- it defers to the Mac's prompt so the
// human picks an option there. The device cannot answer a multi-choice question; it only INDICATES that
// a session is waiting on input -- via the `question` session state (set by the separate `Notification`
// hook) and the "tap to answer on Mac" takeover (§A, FR-BUDDY-8), which on tap focuses that terminal so
// the user answers there. Since PermissionRequest's decision.behavior has no "ask" (allow/deny only),
// defer by emitting NO decision -- an empty body CC reads as "no gate", falling through to its own
// interactive prompt.
{}
```
HTTP 2xx + body, no outer envelope. Hook `timeout` is in **seconds** (config: 600 to cover the
~590 s hold). Non-2xx/timeout = **non-blocking (CC proceeds, fail-OPEN)** -- so the hub MUST return
`deny` within the hold window; never let it hang.

### C.4 Session / statusline — CONFIRMED (CC v2.1.x docs)
`SessionStart`(matcher startup/resume/clear/compact)/`Stop`/`Notification`/`SessionEnd` http hooks =>
buddy idle. Stop body has `stop_reason`; Notification has `message`; SessionEnd carries `session_id`
(clean per-session removal). **Statusline** (`statusLine` = `type:command`)
receives JSON with `session_id` (per-session TOK/CTX aggregation key), `cwd` (attribution basename),
`context_window.{used_percentage,total_input_tokens,total_output_tokens}` (=> buddy
`context_pct`/`tokens`) and `rate_limits.{five_hour,seven_day}.{used_percentage,resets_at}` (=> **Claude
`usage.h5`/`d7`** — now the PRIMARY Claude source, §C.1). The shim **wraps the user's existing
statusline renderer** (forwards the JSON to `127.0.0.1:8765/statusline`, then delegates to the real
command passed as args), so the user's status bar is unchanged. Bind port is the fixed **8765**.

## D. Hub-side policies

- **Short id mapping (`records.h` `BUDDY_ID_LEN`=24 => <=23 chars):** the hub mints a short id per
  permission prompt and maps it to the full Claude Code hook request id. The device only ever sees +
  echoes the short id.
- **Prompt queue (FIFO, one shown at a time):** `buddy_prompt_t` holds the front prompt; additional
  concurrent permission hooks queue FIFO behind it. `qlen` on the BLE frame carries the total pending
  count (incl. the front) so the device can show a `(1 of N)` badge. (`AskUserQuestion` is exempt:
  it is never held, so a question can't squat the queue and block a real permission behind it.)
- **Silent withdraw (resolved on the Mac):** if CC closes the held hook connection -- because the user
  answered the permission in the Mac terminal instead of on the device -- the hub clears the device
  prompt and advances the queue with NO deny and NO "too late" (`watchForClose`/`withdraw`,
  `ClaudeCodeBridge`). The answer applied on the Mac; the device must not claim otherwise.
- **Timing:** design target < 5 s round-trip; ~590 s hold (below Claude Code's ~600 s hook timeout);
  only a queued prompt's own cap expiry denies it (silently); `deny` + label on cap (`tech.md` §8,
  FR-BUDDY-3).
- **Logging:** id + decision + timestamp only. NEVER the command `hint` or any token (`tech.md` §9).

### D.1 Onboarding, lifecycle & error recovery (epic #20 — `docs/research/2026-06-08-hub-ux-audit.md`)

- **First-run setup window (#15):** on first launch (gated by `BeaconFirstRunComplete`) the hub shows a
  setup window with live **Bluetooth / Claude Code hooks / device-connected** rows, each with one-click
  remediation; reachable later via the menu **Setup…**. The hooks **Install** button shells out to
  `build-app.sh install-hooks` (single source of truth for the idempotent `jq` merge) and installs the
  statusline shim to the no-space path `~/.beacon/beacon-statusline` (`build-app.sh` honours a `BEACON_SHIM`
  override so install + detection agree). "Hooks installed" is detected by requiring BOTH the
  `PermissionRequest` hook (`url=http://127.0.0.1:8765/hook`) AND a `statusLine.command` containing the shim
  — not any beacon URL anywhere. Replaces hand-editing `~/.claude/settings.json`.
- **Login item (#16):** `SMAppService.mainApp`, toggled by the menu **Start at login**. The menu reflects the
  REAL registration state (re-read on every menu open), and `.requiresApproval` is surfaced honestly
  (guidance dialog), never a silent false "on".
- **Graceful quit drain (#16):** on quit, every still-held permission prompt is resolved as
  **deny-with-reason** ("Beacon hub is quitting") and the response is flushed to Claude Code BEFORE exit
  (completion-aware: a `DispatchGroup` over the socket-send completions, replied via an idempotent latch from
  the drain or a 1 s safety cap). A `terminating` flag denies any prompt arriving during the drain window, so
  no in-flight CC call is ever left without a responder (fail-OPEN per §C.3 is avoided).
- **Forget device / re-pair (#16):** app-side reset only — cancel the link, drop the cached peripheral,
  rescan. CoreBluetooth has **no API to remove an OS-level bond**, so a truly stuck bond (e.g. keys changed
  after a firmware re-flash) still needs the user's System Settings **Forget This Device**; the menu action
  guides there with a one-click "Open Bluetooth Settings".
- **401 token self-heal (#17):** on a usage 401 the hub re-reads the **CLI-rotated** credential (Claude
  Keychain blob / `~/.codex/auth.json`) and retries the request **exactly once** if the token changed; a
  one-shot guard makes a second 401 unable to loop. Self-heals the common case (active user whose CLI already
  refreshed); an idle, truly-expired token still ends at "…token expired - re-login". A real OAuth
  refresh-token grant is **deferred** (it needs unofficial, reverse-engineered endpoints/client_ids).
- **BLE pairing-failed escalation (#17):** a **first-time** bond (`hadConnection == false`) that fails
  **4 attempts or 25 s** (monotonic clock) escalates to a loud `LinkPhase.pairingFailed` ("Pairing failed" +
  **Try pairing again**) instead of rescanning forever silently; a reconnect blip of a previously-bonded
  device is never escalated. "Try again" (`retryPairing`) resets the budget + rescans, distinct from the
  heavier forget/re-pair path.
