# Provider Architecture: pluggable agents and capabilities

Status: approved design, 2026-07-19. Authority: this doc governs the multi-provider refactor; wire details land in `hub/CONTRACT.md` when implemented.

## Goal

Beacon today hardwires exactly two usage providers (Claude, Codex) and one buddy harness (Claude Code). This design makes both planes pluggable:

- A **provider** is a named agent ecosystem (`claude`, `codex`, later `gemini`, `opencode`, ...) with a stable lowercase id and a display label.
- A provider declares **capabilities**: `usage` (quota windows), `sessions` (live session list), `prompts` (remote approve/deny). Tiered: a provider may support any subset.
- Each provider exposes **two user toggles** in the hub menubar, persisted on the Mac: **Usage** and **Coding Buddy** (buddy = sessions + prompts together). Example: Claude usage on, Claude buddy off, Codex buddy on.
- The device renders whatever enabled providers send; it never hardcodes provider names.

Non-goals (this iteration): out-of-process third-party plugins (the internal event model is shaped so a "generic webhook provider" can be added later without rearchitecture); >2 providers rendered on the usage screen (wire and records carry up to 4; themes render the first 2); on-device toggles.

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Plugin mechanism | Compiled-in Swift protocol (`AgentProvider`) + registry | Simplicity first; type-safe; the existing `UsageProvider` seam proves the pattern. Out-of-process API is additive later. |
| Toggle location | Hub menubar, `UserDefaults` | Providers run on the Mac; toggling starts/stops pollers and hook handling live. |
| Usage wire format | Clean cutover to a provider array (see below) | Pre-1.0, hub+firmware ship together, web flasher makes reflash trivial. No dual emission. |
| Buddy wire format | Unchanged shapes + additive optional `agent` field on session rows and prompt | Already agent-agnostic; `agent` enables future badging. |
| Second harness | Codex CLI | Ships a Claude-compatible hooks system (stable, default-on): `PermissionRequest`, `SessionStart`, `SessionEnd`, `Stop`, `UserPromptSubmit` with the same stdin-JSON/stdout-decision contract. Full tier feasible. |
| Buddy disabled semantics | Pass-through | Hub returns no verdict on permission hooks; the harness falls back to its own interactive prompt. Never auto-deny because a toggle is off. |

## Wire protocol changes (hub/CONTRACT.md)

### Usage block (BREAKING, clean cutover)

Old: `"usage":{"claude":{...},"codex":{...}}`. New:

```json
{"v":1,"usage":{"providers":[
  {"id":"claude","label":"CLAUDE","h5":{"pct":24,"reset":1717600000},"d7":{"pct":24,"reset":1717800000},"stale":true},
  {"id":"codex","label":"CODEX","h5":{"pct":1,"reset":1717590000},"d7":{"pct":29,"reset":1717800000}}
]}}
```

- `providers`: 0..4 entries, hub display order. Only providers with the usage toggle ON appear.
- `id`: stable lowercase ascii, <=12 chars. `label`: display string, <=10 chars, uppercase preferred.
- `h5`/`d7`/`stale` semantics unchanged (omitted pct => unavailable; `stale` emitted only when true).
- Old firmware fails this block's parse and shows usage unavailable; flash matching firmware. Documented in CONTRACT migration notes.

### Sessions frame + buddy prompt (additive)

```json
{"v":1,"sessions":[{"id":"s3","agent":"codex","label":"api · main","state":"working","ts":1719399860}]}
"prompt":{"id":"p07","agent":"claude","tool":"Bash","hint":"rm -rf /tmp/build","qlen":2}
```

- `agent` = provider id; optional on the wire, always emitted by the new hub. Firmware stores it (cap 12 chars); views may ignore it for now.
- Session `id` (`sN`) and prompt `id` (`pN`) remain hub-minted, globally unique across providers; device commands (`permission`, `open`) are unchanged and the hub routes them back to the owning provider.
- `buddy.running/waiting` count across all buddy-enabled providers. `tokens`/`context_pct` stay (populated only by providers with a statusline-equivalent source; 0 otherwise).

## Hub architecture (Swift)

### BeaconHubKit (pure, unit-tested)

New `Providers.swift`:

```swift
public struct ProviderCapabilities: OptionSet { usage, sessions, prompts }
public struct ProviderDescriptor { let id: String; let label: String; let capabilities: ProviderCapabilities }
public struct EnabledCapabilities: Equatable { var usage: Bool; var buddy: Bool }   // user toggles
public struct ProviderSettings { /* pure defaults-all-on + apply/read logic over a key-value store protocol, so UserDefaults stays in the app target */ }
```

`Protocol.swift`: replace `Usage{claude,codex}` with

```swift
public struct UsageEntry: Codable, Equatable { var id: String; var label: String; var h5: UsageWindow; var d7: UsageWindow; var stale: Bool? }
public struct Usage: Codable, Equatable { var providers: [UsageEntry] }   // StatusFrame.usage type unchanged in name
```

`Session` and `BuddyPrompt` gain `var agent: String?` (encoded only when non-nil).

New `ProviderMux.swift` (pure aggregator, the heart of the design):

- Registers provider outputs keyed by provider id; holds latest `ProviderUsage?` per usage provider and per-provider session/prompt state.
- Owns global short-id minting: sessions via the existing `SessionRegistry` (extended so each tracked session carries `(providerID, nativeKey)`), prompts via a `PromptBroker` mapping `pN -> (providerID, nativeID)` with the existing FIFO front-prompt queue semantics (qlen counts across providers).
- Applies toggles: disabled capability => provider's data excluded from output and its prompts never held.
- Emits: `Usage` (array, enabled providers in registration order), merged `[Session]` newest-first, `BuddyState`, and routes `permission`/`open` device commands to the owning provider by short id.

### beacon-hub (app target)

```swift
protocol AgentProvider: AnyObject {
    var descriptor: ProviderDescriptor { get }
    func start(sink: ProviderSink)               // sink = mux-facing event callbacks
    func setEnabled(_ caps: EnabledCapabilities) // live toggle; provider stops holding prompts / polling as needed
    func stop()
    func resolvePrompt(nativeID: String, approve: Bool) -> ResolveOutcome
    func focusSession(nativeKey: String) -> Bool // sessions capability; false = unsupported
}
```

- `LocalIngestServer`: generalization of the `ClaudeCodeBridge` NWListener on :8765. Providers register path-prefixed routes (`/hook`, `/statusline`, `/session` stay Claude's for shim back-compat; Codex uses `/codex/hook`). One listener, one port file.
- `ClaudeCodeProvider`: wraps the existing bridge logic (HTTP hooks, statusline usage + poll gating, Keychain fallback poller, session host context). Capabilities: usage+sessions+prompts.
- `CodexProvider`: usage from the existing `~/.codex/auth.json` poller; sessions+prompts from Codex hooks (below). Capabilities: usage+sessions+prompts.
- `UsagePoller`: iterates usage-capable, usage-enabled providers from the registry instead of two fixed slots. Per-provider retention (`UsageReducer`) keyed by id.
- `HubPanel`: one card per registered provider (dynamic), each with Usage / Coding Buddy toggles shown per capability; toggles write `ProviderSettings` and call `setEnabled` live.
- `AppDelegate`: builds the registry `[ClaudeCodeProvider, CodexProvider]`, wires mux -> BLE frames, device commands -> mux.

### Codex buddy adapter

Codex hooks are command-type (spawn argv, JSON on stdin, decision on stdout). Bridge them with one shim, `hub/statusline-shim/beacon-codex-hook`:

- Reads stdin JSON, POSTs it to `http://127.0.0.1:<port>/codex/hook` (port from `~/.beacon-hub/port`), prints the response body to stdout.
- For `PermissionRequest` the POST blocks (curl timeout ~590s) until the hub resolves; hub replies with the Claude-compatible decision JSON (`hookSpecificOutput.decision.behavior: allow|deny`) or `{}` for no-verdict (falls through to the Codex TUI prompt). Hub offline / connect refused => shim prints nothing and exits 0 (fail-open to the TUI, matching pass-through semantics).
- Lifecycle events map to session state: `SessionStart` => register (label from `cwd` basename + git branch when available), `UserPromptSubmit` => working, `Stop` => attention/idle (reuse the existing registry transition rules), `SessionEnd` => remove.
- Installer: managed block in `~/.codex/config.toml` `[hooks]` wiring the five events to the shim. Verified TOML shape (openai/codex `codex-rs/config/src/hook_config.rs` @ 0fb559f0): `[[hooks.<Event>]]` matcher groups, each with optional `matcher` and `hooks = [{ type = "command", command = "<path>", timeout = <sec> }]`. Set `timeout = 590` on `PermissionRequest`; omit for the fire-and-forget lifecycle events. Detection mirrors `HooksDetection`.
- Verified decision contract (`codex-rs/hooks/src/{schema.rs,engine/output_parser.rs,events/permission_request.rs}`): stdin is snake_case `{session_id, turn_id, cwd, hook_event_name, model, permission_mode, tool_name, tool_input, ...}`; stdout decision is `{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"|"deny","message":"..."}}}` (byte-compatible with the hub's existing `HookResponse.permission`); empty stdout or `{}` = no verdict (falls through to the TUI); exit 2 + stderr = deny; any deny wins the fold. `SessionStart` input carries `source: startup|resume|clear|compact`; `SessionEnd` carries `reason`.
- Known risk: Codex `[hooks.state]` tracks `{enabled, trusted_hash}` per hook; a freshly installed hook may need a one-time trust step in Codex. The installer must either write the expected state or document the trust prompt; the executor verifies the actual first-run behavior.

## Firmware changes

- `records.h`: `#define USAGE_PROVIDERS_MAX 4`, `#define USAGE_ID_LEN 13`, `#define USAGE_LABEL_LEN 11`; `usage_provider_t` gains `char id[]`, `char label[]`; `usage_rec_t` becomes `{ record_hdr_t hdr; uint8_t count; usage_provider_t p[USAGE_PROVIDERS_MAX]; }`. `buddy_session_t` and `buddy_prompt_t` gain `char agent[USAGE_ID_LEN]` (empty when absent).
- `hub_proto.cpp`: parse the `providers` array (cap 4, truncate strings, tolerate absent windows); parse optional `agent` fields. Malformed usage block => reject block, keep last (parity with current behavior).
- `datastore`: pass-through of the new struct; no semantic change.
- Usage views (7 themes): render `count` providers (0, 1, or 2; >2 renders the first 2) with labels from the record instead of hardcoded "claude"/"codex" strings. Layout stays the current grid; a single provider centers or occupies the top row per theme's existing geometry.
- Buddy views (7 themes): replace "CLAUDE" branding strings with neutral "AGENTS" (casing per theme convention). Screen id string `CLAUDE` stays (internal, persisted in NVS ordering).
- Native tests: update `hub_proto` and datastore tests for the new shapes; table-driven cases for 0/1/2/4 providers, truncation, malformed block.

## Toggle semantics (normative)

| Toggle | OFF effect |
|---|---|
| Usage | Provider excluded from `usage.providers`; its poller stops. |
| Coding Buddy | Provider's sessions and prompts excluded from frames; permission hooks answered with no-verdict immediately (pass-through to the harness's own UI); pending held prompts for that provider are released pass-through on toggle-off. |

Defaults: all capabilities enabled (preserves current behavior on upgrade).

## Verification

- `cd hub && swift test` (kit + app logic) and `swift build`.
- `cd firmware && pio test -e native` (records/proto/datastore tables).
- Manual smoke (documented, not CI): hub menubar shows two provider cards; toggling Claude buddy off passes a CC PermissionRequest through to the terminal.
