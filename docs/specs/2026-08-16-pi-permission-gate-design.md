# Pi Permission Gate — Design

Status: approved in design session 2026-08-16; hardened after cross-model review the same day. Supersedes the mirror-only limitation recorded in `hub/CONTRACT.md` §C.6 (pi keeps sessions mirroring; this adds the prompts plane).

## Goal

Approve/deny pi tool calls from the Beacon device, working both with and without `@gotgenes/pi-permission-system` (pps). Hard degradation guarantee: when the hub is not running or the device is not connected, pi must never see an error, never hang, and never have a tool denied by infrastructure — the ask simply falls back to the local TUI (or, with pps, to pps's own prompt). Beacon absence-of-decision is always pass-through, never deny.

## Facts this design rests on (pi 0.84.2, pps 25.0.0, verified against installed source)

- A `tool_call` handler can be held async; returning `{block:true}` cancels the call; the first block short-circuits later handlers.
- `ctx.ui.select(title, options, {signal})` works inside a held `tool_call`, is programmatically dismissable via `AbortController`, and resolves `undefined` on cancel — unlike `confirm`, which collapses Esc and "No" into `false`, it distinguishes cancellation from denial.
- pps (MIT) exposes an external-resolver seam: `PermissionsService.registerAuthorizer(name, fn)` + ordered `authorizerChain` in its `config.json` (links evaluated in config order); discovery via `getPermissionsService()` and the `permissions:ready` event. Authorizers return allow/deny/defer.
- pps's delegation envelope (`delegation-envelope.ts`) downgrades an authorizer `allow` to `defer` on the `path` and `external_directory` surfaces — a remote Approve cannot stick there; only deny/defer are honored.
- Without any permission extension pi is yolo (no native ask UI). In print/JSON mode (`ctx.hasUI === false`) the no-op UI resolves dialogs to a refusal — an unguarded prompt would fail closed by accident. pps has its own headless policy (parent authorizer for subagents, deny otherwise) which Beacon must not disturb.

## Decision summary

| Decision | Choice |
| --- | --- |
| With pps installed | pps stays the policy brain (user's rules untouched). Beacon registers as an authorizer (appended to `authorizerChain`, preserving existing links). Device Approve/Deny applies only on surfaces where the delegation envelope honors authorizer allow (e.g. bash/tool surfaces); on `path`/`external_directory` asks Beacon defers immediately and pps prompts locally as today. |
| Without pps | `beacon.ts` gates via its own `tool_call` handler with a minimal built-in policy: ask for `bash` and for `write`/`edit` targeting paths outside the project cwd; allow everything else. Containment uses canonicalized paths (`path.resolve` + `path.relative`, symlink-resolved via the nearest existing ancestor) with tests for `..`, sibling-prefix, symlink-escape, and missing-target cases. |
| Coexistence guard | The standalone gate disables itself whenever pps is detected in-process (guarded dynamic discovery of the pps service + `permissions:ready`; re-checked across `/reload`). Ships in Phase 1 so no window of double-gating ever exists. |
| Prompt surfaces | Device prompt and TUI `ctx.ui.select` (Allow / Deny) race, first answer wins, loser dismissed via its `AbortController`. `undefined` (Esc/cancel) counts as no answer from that surface, and `ctx.signal` abort dismisses both. |
| Truthful device acks | Two-phase: the hub delivers the device decision to the extension's long-poll, the extension reconciles the race and POSTs a commit (`applied` or `lost-race`) within a bounded window, and only then does the hub ack the device (`ok:true` / `ok:false`). The frozen BLE ack contract is honored — the device never sees `ok:true` for a decision that lost to the TUI. |
| Device flow | Reuse of the existing `buddy.prompt` contract (25 s device expiry, qlen). Pi's descriptor gains `.prompts`; zero firmware changes. |
| Authorizer chain hookup | The hub's Pi Set-up chip appends `beacon` to pps `config.json` `authorizerChain` (timestamped backup, idempotent). When pps is detected, the Ready state additionally requires the chain entry, so a later pps install flips Settings back to "Set up". |
| Non-interactive runs | `ctx.hasUI === false`: the standalone gate does not gate (matching pi's yolo baseline for headless runs), and the pps authorizer returns defer so pps's own headless policy applies unchanged. |

## Failure ladder (the load-bearing requirement)

Every rung fails toward a working local decision; Beacon absence-of-decision is pass-through, never deny. Nothing is ever printed to the pi session.

| Condition | Behavior |
| --- | --- |
| Hub not running | One probe POST with `AbortSignal.timeout(400)` fails => circuit breaker opens (skip hub for 60 s) => ask is TUI-only. With pps: defer, so pps's own prompt runs. |
| Hub up, device not connected | Hub replies `{device:false}` immediately (it knows link state) => TUI-only, no long-poll held. |
| Hub up, device connected | Extension long-polls the decision while `ctx.ui.select` shows in the TUI; first answer aborts the other side, then the commit step runs. |
| Hub quits / buddy toggled off / hold cap reached mid-prompt | The pi route resolves held prompts as pass-through (`{}`-equivalent `unavailable` result), NOT deny — unlike Codex's fail-closed drain. Extension falls back to TUI / defer. Deny is reserved for an explicit human decision. |
| Device prompt expires (25 s) unanswered | Device drops it; hub keeps holding until decision, TUI answer, or cap — the TUI stays live throughout. |
| User aborts the agent (`ctx.signal`) | Both surfaces dismissed, no verdict; pi's own cancellation proceeds. With pps: defer. |
| Hub answers deny (human tapped Deny) | `{block:true, reason:"Denied on Beacon"}` after commit. |
| Any unexpected exception in the gate | Caught; with pps => defer, without pps => allow (yolo baseline). Never crash the extension, never emit unhandled rejections. |

## Architecture

```text
                       tool_call
                           |
             pps detected in-process?
            yes            |            no
             v             |             v
   pps policy engine       |     beacon.ts built-in policy
   (user's config.json)    |     (ask: bash, write/edit outside cwd;
   allow/deny: silent      |      standalone gate self-disabled when
   ask: authorizer chain   |      pps is detected)
     -> beacon authorizer  |     allow: silent
        (defer instantly   |     ask: same prompt path
         on envelope-      |            /
         excluded surfaces)|           /
             \             |          /
              v            v         v
      race: hub/device long-poll  <->  ctx.ui.select (Allow/Deny)
      (each surface owns an AbortController; loser dismissed;
       extension commits the winner before the device is acked)
      any failure rung => TUI-only / pps defer => never deny
```

Hub side: `HookBuddyProvider` for pi gains a held-prompt path on a new `/pi/permission` route (request + long-poll decision + commit), reusing the `PromptBroker` front-prompt queue but with pi-specific operational outcomes: quit-drain, cap, and toggle-off resolve as `unavailable` pass-through instead of Codex's deny. Descriptor becomes `[.sessions, .prompts]`.

## Delivery phases

1. **Phase 1 — standalone gate**: built-in policy + pps-detection step-down in `beacon.ts`, `/pi/permission` route with pass-through operational outcomes and the commit step, device prompt plumbing, failure ladder, tests. Ships value without pps and is inert (mirror-only) when pps is present.
2. **Phase 2 — pps authorizer**: guarded service discovery states (absent / present-ready / present-unavailable, across load order and `/reload`), authorizer registration with envelope-aware surface scoping, installer `authorizerChain` append + backup + chain-aware Ready state.

## Test plan

- Host (Swift): pi prompt route hold/decision/commit semantics; quit-drain, cap, and buddy-toggle resolving as pass-through (contrast test against Codex's deny); crossing-ack (`lost-race` => device `ok:false`); CONTRACT frame shape for `buddy.prompt.agent == "pi"`; installer chain append idempotency + backup + Ready-requires-chain-entry.
- Extension (Node harness, extends the #149 smoke): loads clean with no unhandled rejections; policy classification table (bash / inside write / `..` escape / sibling-prefix / symlink-escape / missing target); stub-hub failure rungs (down, never-responds, malformed reply, `{device:false}`, deny, unavailable, circuit-breaker open/close); race outcomes (TUI-wins => hub commit `lost-race`, device-wins => TUI dismissed); pps-detected step-down (stub service present => gate inert).
- Manual: end-to-end approve and deny from the device; hub-quit mid-prompt; device-off mid-prompt; pps installed end-to-end (bash ask answered from device, external-path ask stays local).

## Out of scope

- Usage plane for pi (separate concern, unchanged).
- Mirroring pps prompts Beacon does not own (no question card for envelope-excluded asks in this iteration).
- Replacing or forking pps; upstream PRs (e.g. relaxing the delegation envelope) may follow later but are not required.

**2026-08-16 implementation note:** Pi's commit-before-ack flow widens `ProviderMux` with an asynchronous resolve callback while Claude and Codex retain their synchronous path. After forwarding a device choice, the pi provider holds the device ack for a 2 s extension commit window; `applied:true` yields `ok:true`, while a lost race or missing commit yields `ok:false`.
