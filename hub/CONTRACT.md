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
{"v":1,"cmd":"launch","text":"run the tests"}
{"v":1,"ack":"p07","ok":true}
{"v":1,"err":"unknown_prompt_id","id":"p07"}
```
- `id` echoes the hub-minted short id (see §D). The hub maps it back to the real hook request id.

## C. Upstream shapes (DRAFT from research — REPLACE with real captures in P2-0)

### C.1 Claude usage — `GET api.anthropic.com/api/oauth/usage`
Headers: `Authorization: Bearer <tok>`, `anthropic-beta: oauth-2025-04-20`. Token: Keychain
`Claude Code-credentials` (JSON; access token at `claudeAiOauth.accessToken`; refresh token + expiry
also present — **capture the exact key names for D2 refresh**). Draft body:
```json
{"five_hour":{"utilization":24.0,"resets_at":"2026-06-05T14:20:00Z"},
 "seven_day":{"utilization":51.0,"resets_at":"2026-06-07T23:59:00Z"}}
```
Normalizes to `usage.claude` (`utilization`->`pct`, ISO `resets_at`->epoch).

### C.2 Codex usage — `GET chatgpt.com/backend-api/wham/usage`
Headers: `Authorization: Bearer <tok>`, `chatgpt-account-id: <id>`. Token: `~/.codex/auth.json`
(`tokens.access_token`, `tokens.account_id`). Local fallback (D1): `~/.codex/sessions/**/rollout-*.jsonl`
`rate_limits` — **capture the exact field path in P2-0**. Draft body:
```json
{"rate_limit":{"primary_window":{"used_percent":1.4,"reset_at":1717590000},
               "secondary_window":{"used_percent":29.0,"reset_at":1717800000}}}
```
Normalizes to `usage.codex`.

### C.3 Claude Code permission hook (`PreToolUse` / `PermissionRequest` http hook) — DRAFT
**Capture both hook names in P2-0** and confirm whether they alias + the exact request-id, tool, and
command-hint field names, and the exact **response** shape the hook expects. Draft request body:
```json
{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"rm -rf /tmp/build"}}
```
Draft response (fail-closed default is `deny`):
```json
{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow"}}
```

### C.4 Session / statusline — DRAFT
`SessionStart`/`Stop`/`Notification` http hooks => buddy idle (running/waiting/entries). Statusline
shim POSTs the statusline JSON => `tokens`/`context_pct`. **Capture the exact statusline + session
field names in P2-0** (`docs/research` §2.2).

## D. Hub-side policies

- **Short id mapping (`records.h` `BUDDY_ID_LEN`=24 => <=23 chars):** the hub mints a short id per
  permission prompt and maps it to the full Claude Code hook request id. The device only ever sees +
  echoes the short id.
- **One prompt at a time:** `buddy_prompt_t` holds a single prompt. A second concurrent permission
  hook is queued FIFO (or auto-denied+labeled to avoid stacking two ~25 s holds) — see
  `ClaudeCodeBridge`.
- **Timing:** design target < 5 s round-trip; ~25 s fail-closed cap (below Claude Code's ~30 s hook
  timeout); cap => `deny` + label (`tech.md` §8, FR-BUDDY-3).
- **Logging:** id + decision + timestamp only. NEVER the command `hint` or any token (`tech.md` §9).
