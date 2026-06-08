# P2 — Hub + AI (BLE) — Design Spec

> **What this is:** the design for P2 — the macOS hub app, the device-side BLE `HubLink`
> implementation, and the wiring that feeds the already-built AI Usage + Coding Buddy screens
> with real data. Authority: `prd.md` §5.5/5.6/5.9/5.10 + §7, `tech.md` §4/§6/§7/§8/§9. Where this
> spec and `tech.md` disagree on *how*, `tech.md` wins; this spec only **expands** the frozen §7
> protocol with the details an implementation needs (pairing, framing edge cases, hook ingestion).
>
> **Companion:** `docs/plans/2026-06-08-p2-hub-ai-plan.md` (the chunked implementation plan).

## 0. BLUF

P0 froze the contracts P2 builds against (`HubLink` interface, `usage_rec_t`/`buddy_rec_t`,
`screen_state_t`, the §7 BLE frame schema). P1 proved the device-direct plane. **The AI Usage and
Coding Buddy screens already exist** (7 bespoke views each) and already render from
`ds_get_usage()` / `ds_get_buddy()` with full state handling (loading/live/stale/hub-offline). They
run today on `dev_seed` fake data, and the buddy Approve/Deny buttons call a stub that logs
`"hub round-trip is P2"`.

P2 adds the **hub plane** end to end:

```
  macOS HUB (Swift menubar, CBCentral)                       DEVICE (ESP32, CBPeripheral)
  +--------------------------------------+                  +--------------------------------+
  | UsagePoller  Claude(Keychain)+Codex   |   status frame   | HubLinkBle (Bluedroid NUS)     |
  |   - normalize -> usage{h5,d7}x2        | ---TX notify---> |  RX cb enqueues bytes          |
  | ClaudeCodeBridge  http hooks server    |                  |  loop(): reassemble \n         |
  |   - PreToolUse(blocking) -> prompt     |                  |   -> onFrame -> parse JSON     |
  |   - statusline/Session  -> idle state  | <--RX write----  |   -> ds_set_usage/ds_set_buddy |
  | StatusFrameBuilder  merge -> NDJSON    |   command frame  | decide/launch -> HubLink.send  |
  | Menubar: link status / pairing         |                  | disconnect -> ds_set_hub_offline|
  +--------------------------------------+                  | hub_task (Core-0) pumps loop() |
   holds ALL provider secrets (Keychain / ~/.codex);         +--------------------------------+
   only computed pct/reset + buddy text cross BLE
```

Nothing on the device holds a provider token. The protocol, characteristics, and JSON shapes are
**already frozen** in `tech.md` §7 — P2 implements them; it does not redesign them.

## 1. Scope

| In P2 | Out (later / non-goal) |
|---|---|
| Device Bluedroid `HubLink` impl (NUS peripheral, bonded LESC, framing, send queue) | NimBLE (banned, `tech.md` §5); LAN-WS fallback (kept abstract, not built) |
| Device wiring: `onFrame` => DataStore; disconnect => hub-offline; buddy decide/launch => `send()` | New device screens or view layouts (the 14 views already exist) |
| macOS hub: usage pollers, Claude Code hook bridge, BLE central, menubar | STT/voice (FR-VOICE, Explore); Windows/Linux hub |
| Heap re-measure under active bonded BLE + cert TLS + LVGL (`tech.md` §8, closes the coexistence proof) | Battery-current power profiling (deferred, `tech.md` §8) |
| `hub/CONTRACT.md` recorded fixtures (usage + hook payloads), captured at P2-0 | Answering `AskUserQuestion`, "don't ask again", typing into a live TUI (FR-BUDDY-5) |

## 2. Locked decisions (owner-confirmed 2026-06-08; D2/D3/D4 signed off at the verification gate)

| # | Decision | Choice | Rationale / alternative |
|---|---|---|---|
| **D1** | Codex usage source | **Network** `GET chatgpt.com/backend-api/wham/usage` primary; local `~/.codex/sessions/**/rollout-*.jsonl` `rate_limits` as offline fallback | Network is freshest + matches the Claude path; the local cache only updates when Codex runs (`research` §2.1). |
| **D2** (confirmed) | Claude/Codex token expiry | **Retry once, else show error.** Use the stored access token; on **401**, attempt **one** refresh via the stored refresh-token. If refresh fails: emit that provider's windows as `null` (device shows "--") AND surface the actionable reason in the **menubar** (e.g. "Claude token expired - re-login") | Owner chose retry-once + honest error. OAuth tokens expire ~hourly; without refresh, usage blanks whenever the CLI is idle. **Refresh field names + endpoint are unverified - capture in P2-0 before implementing (§4.2).** The device record has one hdr (no per-provider error state), so "unavailable" is expressed as `null` windows on-device + the error reason on the Mac (where it's actionable). |
| **D3** (confirmed) | BLE pairing UX | **Best standard security that stays seamless.** Mandatory: **LE Secure Connections + bonding** (allowlist, encrypted chars). Preferred: **passkey** (device = DisplayOnly, Settings "Pair" overlay shows a 6-digit code; macOS pairing is OS-mediated - CoreBluetooth triggers encryption and macOS shows the system dialog, the hub does not handle the passkey in code). **P2-A proves the device-DisplayOnly + Mac-passkey UX on real hardware and decides**: keep passkey if seamless; **fall back to Just-Works bonding** (still bonded + encrypted, no MITM) if the passkey flow is clunky on core 3.3.x / macOS | Owner chose "best standard practice balanced with seamless UX". LESC+bonding is the modern BLE standard either way; the passkey is the MITM upgrade, gated on it not wrecking the UX. |
| **D4** (confirmed) | Swift project shape | **Xcode project** under `hub/` producing a `.app` bundle (`LSUIElement`, SwiftUI `MenuBarExtra`, macOS 13+) | Owner chose the most seamless build/run experience. `MenuBarExtra` agent app needs a bundle + Info.plist; Xcode is the path of least resistance. Needs Xcode on the owner's Mac (present). |
| **D5** | Claude Code -> hub transport | Claude Code **`http` hooks** POST to `http://127.0.0.1:<port>` on the hub's local server; the permission hook is held open (blocking) until the device decides — **design target < 5 s round-trip** (`tech.md` §8) — with a **fail-closed cap at ~25 s** (sits below Claude Code's ~30 s hook timeout) => returns `permissionDecision`; cap reached => `deny` + label. Handle **both** `PreToolUse` and `PermissionRequest` hook names | `research` §2.2 / `tech.md` §7.3/§8, FR-BUDDY-3. The ~25 s is the ceiling, not the SLA. Localhost only; no inbound network. Statusline via a shim script (D6). |
| **D6** | Token/context metrics | Hub ships a tiny **statusline shim** the owner sets as their Claude Code `statusLine` command; it forwards the statusline JSON to the hub and prints the original line. Session/idle via `SessionStart`/`Stop`/`Notification` http hooks | Statusline is the only surface with live token/context (`research` §2.2). |
| **D7** | Launch (FR-BUDDY-4, SHOULD) | **Preset commands only** in v1 (device has no keyboard); device sends `{cmd:"launch","text":"<preset>"}`, hub runs `claude -p` in a configured working dir. Free-form/dictation deferred to P3+ | Avoids an on-device keyboard; matches FR-BUDDY-4 "text/dictated later". Last, optional chunk. |
| **D8** | `loop()` pump + callback context | Dedicated **Core-0 `hub` task** at ~50 Hz pumps `HubLink::loop()`. Bluedroid GATT RX/notify callbacks run in the **BT stack task** and may ONLY copy bytes into a queue + return; `loop()` reassembles frames and invokes `onFrame` (and parses JSON / writes the DataStore) on the `hub` task | Keeps BLE off Core-1 (LVGL) AND honors `hublink.h:9` ("`onFrame` ... Called from loop() context"). `fetch_task` (device-plane) stays separate; both Core-0. |

## 3. Device side

### 3.1 `core/hub_proto.{h,cpp}` — Arduino-free protocol codec (host-testable)

The pure logic, isolated from BLE/Arduino so `pio test -e native` covers it (the P1 precedent:
parse/format TUs are Arduino-free, only `<ArduinoJson.h>` + `records.h` + libc):

- **Frame reassembly:** `hub_reassembler` accumulates inbound bytes, splits on `\n`, emits whole
  frames. Caps a partial frame at a max length (drop + log on overflow — a wedged peer must not OOM).
- **Inbound parse** `hub_parse_status(const char* json, len, usage_rec_t*, buddy_rec_t*) -> bool`:
  - reject unless `v == 1` (unknown major => ignore + log, `tech.md` §7.1).
  - `usage.<prov>.<win>.pct`: JSON `null` => `pct = -1` (matches `usage_window_t`, `records.h:50`);
    `reset` epoch => `reset`. A missing provider/window => that window `null`. **`null` windows mean
    "unavailable" (the view renders "--"), NOT stale** — staleness is a record-level state (§5).
  - `buddy`: `running/waiting/tokens/context_pct/entries[]` (+ `entry_count`); `prompt` present =>
    `prompt.present=true` + copy `id/tool/hint` (truncate to `*_LEN`); **absent `prompt` => idle**
    (`prompt.present=false`).
  - **Prompt id capacity:** `BUDDY_ID_LEN=24` (`records.h:10`) => id buffer holds <= 23 chars. Claude
    Code hook request ids can be longer; the **hub** synthesizes a short BLE-safe id (<= 23 chars) and
    maps it internally to the full hook id (documented in `hub/CONTRACT.md`). The device echoes the
    short id verbatim; the hub resolves it back. The device never sees the full hook id.
- **Outbound build** `hub_build_permission(buf, id, approve)` / `hub_build_launch(buf, text)` => the
  exact §7.1 command JSON, newline-terminated. Echoes the originating (short) `prompt.id` (the hub
  rejects stale/unknown ids with `err`).
- **Ack handling** `hub_parse_ack(...)`: `{ack,ok}` / `{err,id}` — logged; v1 does not retry on `err`.

### 3.2 `core/hublink_ble.{h,cpp}` — Bluedroid `HubLink` implementation

Implements the frozen `HubLink` interface (`core/hublink.h`) via the core `BLE*` wrapper. (The pinned
esp32s3 libs back that wrapper with IDF NimBLE, not Bluedroid — P2 finding, `tech.md` §5; the impl
uses only the stack-agnostic wrapper, no raw `esp_ble_*`):

- **GATT peripheral:** Nordic-UART service `6e400001-...`; RX `...0002` (write/no-resp,
  central->device); TX `...0003` (notify, device->central). Advertise `Beacon-<chipid>`.
- **MTU:** request 247 on connect; never assume >20 B/packet — chunk outbound notifications to
  `mtu-3` and rely on `\n` framing (a frame may span notifies; a notify may hold a fragment).
- **Security:** LESC, bonding, encrypted RX/TX (`esp_ble_gap_set_security_param`); IO cap DisplayOnly
  (passkey shown on device, D3). Allowlist bonded peers; a non-bonded central cannot write RX.
- **Inbound (callback-context rule, D8):** the RX-write callback runs in the **BT stack task** and may
  only **copy the bytes into an inbound FreeRTOS queue and return** — it does NOT reassemble, parse,
  or touch the DataStore. `loop()` (Core-0 `hub` task) drains the queue, feeds `hub_reassembler`, and
  invokes the registered `hub_frame_cb` for each whole frame. This honors `hublink.h:9` ("`onFrame`
  ... Called from loop() context"); `json` is valid only during that callback.
- **Outbound `send()`:** thread-safe — copies the frame into a small outbound FreeRTOS queue and
  returns `true` if enqueued (per the frozen contract: "accepted for transport", not an app-level
  ack); `false` if not connected or the queue is full. `loop()` drains it, notifying in `mtu-3`
  chunks. Safe to call from Core-1 (the buddy decide path, §3.4).
- **Connection state:** GATT connect => link up; disconnect => link down + re-advertise. Exposed via
  `isConnected()`. On disconnect the wiring flips to `ST_HUB_OFFLINE`; on reconnect the hub resends a
  full status frame (`tech.md` §7.1) — the device does not request it.

### 3.3 `core/hub_task.{h,cpp}` — wiring (Core-0)

- Owns the `HubLinkBle` instance; `hublink.begin()` (advertise) + registers the `onFrame` handler.
- **`onFrame` (loop context):** `hub_parse_status` => `ds_set_usage` / `ds_set_buddy`, stamping
  `hdr.last_updated = now_s()` (frame receipt). The setters force `ST_LIVE`/`ERR_NONE` (`datastore.h`).
- **Staleness semantics (honest-state rule).** Stamping at receipt is correct **only because the hub
  asserts freshness**: the status frame's `usage` block carries the hub's **last successful poll**;
  when a provider's poll is failing or older than the staleness threshold, the hub emits `null`
  windows (D2) rather than re-asserting a stale number. Device-side `ST_STALE` (usage `stale_s` = 5
  min, `tech.md` §6) then meaningfully catches the **other** failure: the link is up but frames have
  **stopped** (hub wedged) — `last_updated` stops advancing and the sweep ages the record. So a broken
  poller shows "--" (null), and a silent hub shows stale-with-age; neither shows a fake live number.
- **Disconnect watch (edge-triggered):** on the up=>down edge call `ds_set_hub_offline()` **once**
  (flips both hub records to `ST_HUB_OFFLINE`, preserving last values + age — `ds_set_hub_offline`
  touches only `state`, and the sweep never overwrites `ST_HUB_OFFLINE`, per `datastore.{h,cpp}`).
  Re-arm on reconnect. **`ST_RECONNECTING` is unused in P2**: the device goes
  `ST_HUB_OFFLINE -> ST_LIVE` directly on the first post-reconnect frame (the enum value and the
  views' existing guards are retained for a future LAN-WS path, but P2 produces no `ST_RECONNECTING`).
- **Pump:** `hublink.loop()` at ~50 Hz. Tracks a **running min `int_free`** (sampled every iteration,
  not a once-per-~10 s snapshot) for the heap re-measure (§7) under an active link.
- Replaces the hub-plane half of `dev_seed`: `BEACON_DEV` still fakes usage/buddy for offline UI work;
  the default build runs `hub_task`.

### 3.4 Screen wiring — buddy decide path (3 callback shapes, NOT one stub)

The 7 buddy views do **not** share one callback. Verified shapes:
- `decide_cb(lv_event_t*)` with `approve` in the event user-data: **editorial, blueprint, analog,
  oscilloscope** (4 views). Only `buddy_editorial.cpp:13` carries the `"hub round-trip is P2"` stub;
  the other three already `LOGI` their own line.
- `decide_clear()` + `on_deny`/`on_approve`: **calm** (1 view).
- `decide(bool)` + `deny_cb`/`approve_cb`: **led, hud** (2 views).

P2 adds one shared helper `hub_send_permission(const char* id, bool approve)` (in `hub_task`, wrapping
`hub_build_permission` + `hublink.send`) and edits **each of the 7 views' existing decide path** to
call it — 7 bespoke edits across the 3 shapes, not a mechanical find-replace. Each edit:
- builds + `send()`s the decision for `b.prompt.id`;
- **clears `prompt.present` locally ONLY if `send()` returned true** (enqueued). If `send()` fails
  (disconnected / queue full), leave the prompt visible — the user can retry, and the hub's next frame
  is authoritative. The optimistic clear is just instant feedback; the hub still fail-closes on its own
  ~25 s cap, so the device never runs a permission timer.
- keeps the existing guard that ignores taps while `ST_HUB_OFFLINE`/`ST_RECONNECTING`.

**Launch (D7, optional):** a preset-command affordance => `hub_send_launch(preset)`. Last chunk.

**No view layout changes**, and FR-BUDDY-5 stays satisfied: the views expose only Approve/Deny + idle
stats — no multiple-choice, no "don't ask again", no text entry (verified as an explicit acceptance
check in the plan).

## 4. macOS hub (`hub/`, Swift, macOS 13+)

Minimal menubar agent (`tech.md` §10: "minimal menubar UX with clear connection status"; no
force-unwraps outside tests; isolate token handling; never log token contents).

### 4.1 Modules

- **`UsagePoller`** — every 30–60 s (`tech.md` §6; <= 60 s connected per FR-USAGE-2), behind a
  `UsageProvider` protocol (isolate the unofficial endpoints, `tech.md` §13):
  - **Claude:** OAuth token from Keychain (service `Claude Code-credentials`);
    `GET api.anthropic.com/api/oauth/usage` (`Authorization: Bearer`, `anthropic-beta: oauth-2025-04-20`).
    Map `five_hour.utilization`->`h5.pct`, `resets_at` (ISO)->`h5.reset` (epoch); `seven_day`->`d7`.
  - **Codex (D1):** `~/.codex/auth.json` -> `wham/usage` (`Authorization` + `chatgpt-account-id`);
    `primary_window`->`h5`, `secondary_window`->`d7` (`reset_at` already epoch). Local fallback per D1.
  - **Normalize** to §7.2: `pct` int 0–100 or `null`; `reset` epoch seconds. Unavailable
    provider/window => `null`. Token-refresh per D2 (retry once; on failure emit `null` AND set the
    menubar error reason — the actionable surface is the Mac, the device just shows "--"). **The
    frame's usage block reflects the last successful poll; on prolonged failure emit `null`, never a
    re-asserted stale number** (§3.3).
- **`ClaudeCodeBridge`** — a local HTTP server bound to `127.0.0.1` (ephemeral port, written to a
  known config path the hooks read):
  - **Permission hook (blocking, `PreToolUse` and/or `PermissionRequest`, D5):** on POST, mint a short
    BLE-safe `id` (mapped to the full hook request id), build `prompt{id,tool,hint}`, publish it into
    the buddy block, and **hold the HTTP response open** until the device returns a matching decision
    or the **~25 s fail-closed cap** (target < 5 s); respond with `permissionDecision` (`allow`/`deny`);
    cap reached => `deny` (fail-closed, FR-BUDDY-3) + label.
  - **Concurrent prompts:** the device/`buddy_prompt_t` holds exactly one prompt (`records.h:59-64`),
    so the **hub** serializes: a second permission hook arriving while one is pending is **queued
    FIFO**; if the queue would stack two long holds, the second is **auto-denied + labeled** (default;
    document the exact policy in `hub/CONTRACT.md`). The device side needs no change.
  - **Session/idle:** `SessionStart`/`Stop`/`Notification` http hooks => running/waiting + entries.
    **Token/context:** the statusline shim (D6) POSTs statusline JSON => `tokens`/`context_pct`.
  - **Logging:** hub logs only `id` + decision + timestamp (`tech.md` §9) — **never the `hint`/command
    string** (which carries the actual command, e.g. `rm -rf ...`) and never token contents.
- **`BeaconCentral`** (CoreBluetooth `CBCentralManager`) — scan for the `Beacon` name prefix +
  service UUID; connect; **trigger encryption by accessing an encrypted characteristic — macOS shows
  the OS pairing dialog** (D3; the app does not handle the passkey itself); discover NUS; subscribe TX
  notify; write RX with **`.withResponse`** (acknowledged: hardware-verified that `.withoutResponse`
  packets drop under WiFi+BLE congestion and corrupt multi-chunk frames, §4.3). Auto-reconnect on drop
  (FR-HUB-3). Reassemble inbound acks on `\n`.
- **`StatusFrameBuilder`** — merges the latest usage + buddy state into one §7.1 status frame,
  serializes to newline-delimited JSON, chunks to the negotiated MTU, writes via `BeaconCentral`.
  Sends on change + a heartbeat; **resends a full frame on (re)connect** (the device relies on this).
- **Menubar (`MenuBarExtra`)** — connection status (scanning / connected `<name>` / disconnected),
  last-sync age, the four usage values (mirror), a **Pair** action, Launch working-dir setting, Quit.

### 4.2 `hub/CONTRACT.md` — recorded fixtures (captured at **P2-0**, before P2-A; `tech.md` §7.3)

`tech.md` §7.3 mandates the fixtures **at P2 start** so device parse + hub build are tested against
the **same** real payloads — not guessed field names that churn later. P2-0 captures, redacted:
- **`claude_usage.json`, `codex_usage.json`** — real endpoint responses + the normalized `usage`
  block; **plus the token/refresh field names + refresh endpoint** needed to settle D2.
- **`pretooluse_hook.json`, `permissionrequest_hook.json`, `session_hook.json`, `statusline.json`** —
  real Claude Code hook / statusline payloads (exact field names; confirm whether `PreToolUse` and
  `PermissionRequest` are aliases), + the `buddy` block they map to, + the short-id<->hook-id mapping.
- **`status_frame.ndjson`, `commands.ndjson`** — canonical §7.1 frames the device host tests (§3.1)
  load and the hub serializer is asserted against.

### 4.3 macOS runtime + packaging (hardware-verified 2026-06-08)

Findings from the first on-hardware run, now binding:
- **Signed `.app` bundle, launched via LaunchServices, is mandatory.** macOS TCC reads
  `NSBluetoothAlwaysUsageDescription` only for a LaunchServices-launched, code-signed app; a bare
  `swift run` (or running the binary by path) is **aborted as a privacy violation** — an embedded
  `__info_plist` section is not enough. Ship `Info.plist` + `build-app.sh` (ad-hoc sign + `open`); run
  via `./build-app.sh run` (streams logs to the terminal). A notarized Developer ID build is the later
  distribution step (and the only way "Always Allow" Keychain grants persist across rebuilds).
- **RX writes use `.withResponse`** (acknowledged/flow-controlled). `.withoutResponse` drops under
  WiFi+BLE coexistence congestion and corrupts multi-chunk frames (device logs `bad/ignored frame`).
- **Keychain token is cached** (read once, re-read on 401) so macOS prompts once per run, not per poll.
- **Device firmware ships with `-DBEACON_LVGL_PSRAM`** (LVGL buffers in PSRAM) — required for the BLE +
  WiFi + TLS coexistence to hold the internal-heap floor (§7 / `tech.md` §2).

The device `test_hub_proto` (P2-A) is built against these real fixtures from the start.

## 5. State & failure model (FR-STATE, FR-BUDDY-3)

| Event | Device result |
|---|---|
| Boot, never bonded (default build) | both hub records stay `ST_LOADING` => views show "--" placeholder (honest: nothing was ever live). A "pair a hub" hint lives in Settings/menubar, not in the usage/buddy values |
| Link up, valid frame | `ds_set_usage`/`ds_set_buddy` => `ST_LIVE`; screens render values |
| Provider poll failing / unavailable | hub emits `null` windows => view shows "--", record stays `ST_LIVE` (live-but-unavailable, not stale) |
| Link up but frames stopped (hub wedged) | `last_updated` stops advancing; sweep => `ST_STALE` + age at `stale_s` (5 min) |
| BLE disconnect / hub quit | `ds_set_hub_offline()` => `ST_HUB_OFFLINE`, last values + age (not blank/fake) |
| Reconnect | hub resends full frame => `ST_LIVE` (direct, no `ST_RECONNECTING` in P2) |
| Permission prompt arrives | `prompt.present=true` => buddy view shows tool+hint + Approve/Deny |
| User taps Approve/Deny | `send({cmd:permission,id,decision})`; local clear only if `send()` succeeded; hub resolves the hook |
| No decision before ~25 s cap (hub-side) | hub returns `deny` (fail-closed) + labels; device prompt cleared by the next frame |
| Unknown/stale `id` | hub replies `err`; device logs, no resurrection |
| Malformed / `v != 1` frame | ignored + logged; last good state retained |

`permission` decisions **fail closed**, and the authoritative timeout is **hub-side** (it owns the
blocking hook response). The device clears optimistically (only on a successful enqueue) and never
blocks the LVGL loop or runs its own permission timer.

## 6. Security (`tech.md` §9)

- Provider tokens (Claude Keychain, Codex `~/.codex`) **never leave the Mac**; only computed
  `pct`/`reset` + buddy text cross BLE. The hub never logs token contents **or the command hint**.
- BLE: bonded + LESC-encrypted characteristics + allowlist; per-prompt `id` matching; D3 pairing.
- Hub HTTP server binds `127.0.0.1` only (no inbound network surface).
- Hub logs approve/deny (timestamp + short id) per §9 — never the token, never the command.
- No secrets in the repo; `hub/CONTRACT.md` fixtures are **token-redacted**.

## 7. Heap re-measure (closes the coexistence proof — `tech.md` §2/§8, FR-USAGE acceptance)

The spike proved coexistence only for *advertising + insecure TLS + raw GFX*. P2 MUST re-measure
**min free internal heap** under the worst case: an **active bonded BLE connection + cert-validated
TLS fetch + the full LVGL UI with a heavy theme**.

- **Method:** `hub_task` tracks a **running minimum** of `esp_get_free_internal_heap_size()` sampled
  **every loop iteration** (the existing `fetch_task.cpp:75` log is a once-per-~10 s snapshot — it misses
  short TLS/BLE allocation troughs; the running min does not). Capture the min over a few minutes
  while (a) the hub is connected and pushing frames, (b) a TLS finance fetch runs, (c) the heaviest
  screen is visible. **Pass = >= 60 KB floor holds** (`tech.md` §8).
- **Escape valve (the flag does not exist yet — P2 adds it).** `lvgl_port.cpp:47-57` picks the LVGL
  draw-buffer region **at boot**, before WiFi/BLE exist, so its auto-fallback cannot react to the
  later active-link pressure. P2 adds a real **`-DBEACON_LVGL_PSRAM`** build flag that **forces** the
  buffers into PSRAM regardless of boot-time heap. The re-measure is a **two-build comparison**:
  default (buffers in internal SRAM) vs forced-PSRAM. If the default build breaches the floor under
  load, ship the forced-PSRAM build (slower flush, safe heap) and re-measure to confirm.
- Record the number in the plan's status block and update `tech.md` §2 (replace "not yet measured").
  `lvgl_port.cpp:78` already flags "remeasure at P2".

## 8. Testing

- **Host (`pio test -e native`):** `test_hub_proto` — table-driven over the **P2-0** `hub/CONTRACT.md`
  fixtures (real field names): frame reassembly (split across chunk boundaries, partial + overflow),
  status parse (null windows, missing provider, prompt present/absent, `v != 1` reject, id truncation),
  command build (short-id echo, launch). All Arduino-free; added to `[env:native]` `build_src_filter`.
- **Device:** advertise + **bond from nRF Connect** and prove, before any Swift exists: encrypted
  write is **rejected** before bonding and accepted after; TX subscribe + notify chunking; allowlist;
  reconnect. Then the hub app drives a live link.
- **Hub (Swift):** unit tests for the normalizers (fixture in => §7.2 block out) and the frame builder
  (block in => canonical NDJSON out). BLE + hooks are integration-tested on device.
- **Fault injection (`tech.md` §8):** drop BLE mid-prompt, 401 from a stubbed usage endpoint, kill the
  hub — the UI stays responsive and honest (prompt left visible if `send()` failed; usage to
  hub-offline; no fake live value).

## 9. Owner-confirmation gate (human verification) — RESOLVED 2026-06-08

The three owner decisions are signed off (§2): **D2** = retry-once then show error (device "--" +
menubar reason), **D3** = LESC+bonding with passkey preferred, P2-A decides passkey-vs-Just-Works on
the seamless-UX result, **D4** = Xcode project. Remaining human-gated steps before/within
implementation: **P2-0** (capture real usage + hook payloads on the owner's Mac, §4.2 — needed before
the device codec lands) and the `prd.md` §9 on-hardware acceptance (§8). Everything else follows the
frozen `tech.md` §7 contract.
