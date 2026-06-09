# hub/CONTRACT.md — recorded fixtures (P2-0)

> The shared fixture set so the device codec (`firmware/.../core/hub_proto.cpp` + `test_hub_proto`)
> and the hub (`BeaconHubKit` + its tests) are tested against the **same** payloads (`tech.md` §7.3).
>
> **Status:** the **device-facing frame + commands (§A/§B) are FROZEN** in `tech.md` §7.1 and final.
> The **upstream shapes (§C) are DRAFTS from `docs/research` §2.1/§2.2** — they MUST be replaced with
> **real, token-redacted captures from the owner's Mac** during P2-0 before the usage/bridge chunks
> (P2-D/P2-E) are trusted; the exact field names of the unofficial endpoints + Claude Code hooks are
> only known once recorded. Nothing here may contain a real token.

## A. Hub -> device status frame (FROZEN, `tech.md` §7.1)

Newline-delimited JSON, `"v":1`. `usage` and `buddy` are independently optional (send what changed;
the device keeps an absent block's last values). A null/omitted window `pct` => unavailable ("--").

```json
{"v":1,"usage":{"claude":{"h5":{"pct":24,"reset":1717600000},"d7":{"pct":24,"reset":1717800000}},
                "codex":{"h5":{"pct":1,"reset":1717590000},"d7":{"pct":29,"reset":1717800000}}},
 "buddy":{"running":2,"waiting":1,"tokens":184502,"context_pct":42,
          "entries":["10:42 git push","10:41 yarn test"],
          "prompt":{"id":"p07","tool":"Bash","hint":"rm -rf /tmp/build"}}}
```
- Absent `buddy.prompt` => idle. `pct` is an integer 0..100 or JSON null (device reads null/absent as -1).
- The device codec (`hub_parse_status`) + `test_hub_proto` assert exactly this shape.

## B. Device -> hub commands + hub acks (FROZEN, `tech.md` §7.1)

```json
{"v":1,"cmd":"permission","id":"p07","decision":"approve"}   // or "deny"
{"v":1,"ack":"p07","ok":true}                                // decision applied
{"v":1,"ack":"p07","ok":false}                               // decision did NOT apply (late/superseded)
{"v":1,"err":"unknown_prompt_id","id":"p07"}                 // id the hub never minted
```
- `id` echoes the hub-minted short id (see §D). The hub maps it back to the real hook request id.
- `ok:false` = the device decided but the hub had already resolved the prompt (e.g. the 25s fail-closed
  cap fired first, or it was superseded). The device must surface this, not treat it as success.

## C. Upstream shapes (DRAFT from research — REPLACE with real captures in P2-0)

### C.1 Claude usage — statusline `rate_limits` (PRIMARY); `oauth/usage` (FALLBACK, 429s)
**Hardware finding:** `GET api.anthropic.com/api/oauth/usage` now **returns 429** (Anthropic's
subscription-limits change), so it is only a best-effort fallback. **Live Claude usage comes from the
statusline `rate_limits` (§C.4)** — first-party, no token. Fallback endpoint headers:
`Authorization: Bearer <tok>`, `anthropic-beta: oauth-2025-04-20`, `User-Agent`. Token: Keychain
`Claude Code-credentials` (access token at `claudeAiOauth.accessToken`; refresh/expiry also present).
Fallback body (when it answers): `{"five_hour":{"utilization":24.0,"resets_at":"...ISO"},"seven_day":{...}}`
=> `usage.claude` (`utilization`->`pct`, ISO `resets_at`->epoch).

### C.2 Codex usage — `GET chatgpt.com/backend-api/wham/usage`
Headers: `Authorization: Bearer <tok>`, `chatgpt-account-id: <id>`. Token: `~/.codex/auth.json`
(`tokens.access_token`, `tokens.account_id`). Local fallback (D1): `~/.codex/sessions/**/rollout-*.jsonl`
`rate_limits` — **capture the exact field path in P2-0**. Draft body:
```json
{"rate_limit":{"primary_window":{"used_percent":1.4,"reset_at":1717590000},
               "secondary_window":{"used_percent":29.0,"reset_at":1717800000}}}
```
Normalizes to `usage.codex`.

### C.3 Claude Code permission hook (`PermissionRequest`, primary; `PreToolUse`, back-compat) — CONFIRMED (CC v2.1.x docs)
Claude Code supports native **`"type":"http"`** hooks (no curl forwarder needed). `PreToolUse` and
`PermissionRequest` are **distinct** events, and **`PermissionRequest` is the one Beacon hooks**:
`PreToolUse` fires on **every** tool call, so holding it open ~25 s would block routine `Read`/`Grep`
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
// human picks an option there; the device shows only a passive "asking a question" feed entry. Since
// PermissionRequest's decision.behavior has no "ask" (allow/deny only), defer by emitting NO decision --
// an empty body CC reads as "no gate", falling through to its own interactive prompt.
{}
```
HTTP 2xx + body, no outer envelope. Hook `timeout` is in **seconds** (config: 35 to cover the device's
~25 s window). Non-2xx/timeout = **non-blocking (CC proceeds, fail-OPEN)** -- so the hub MUST return
`deny` within the window; never let it hang.

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
- **One prompt at a time:** `buddy_prompt_t` holds a single prompt. A second concurrent permission
  hook is queued FIFO (or auto-denied+labeled to avoid stacking two ~25 s holds) — see
  `ClaudeCodeBridge`. (`AskUserQuestion` is exempt: it is never held, so a question can't squat the
  slot and auto-deny a real permission behind it.)
- **Silent withdraw (resolved on the Mac):** if CC closes the held hook connection — because the user
  answered the permission in the Mac terminal instead of on the device — the hub clears the device
  prompt and frees the slot with NO deny and NO "too late" (`watchForClose`/`withdraw`,
  `ClaudeCodeBridge`). The answer applied on the Mac; the device must not claim otherwise.
- **Timing:** design target < 5 s round-trip; ~25 s fail-closed cap (below Claude Code's ~30 s hook
  timeout); cap => `deny` + label (`tech.md` §8, FR-BUDDY-3).
- **Logging:** id + decision + timestamp only. NEVER the command `hint` or any token (`tech.md` §9).
