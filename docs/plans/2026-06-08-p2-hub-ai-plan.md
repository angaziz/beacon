# P2 — Hub + AI (AI Usage + Coding Buddy over BLE) — Implementation Plan

> Branch: `feat/p2-hub-ai`. Authority: `prd.md` §5.5/5.6/5.9/5.10 + §7, `tech.md` §4/§6/§7/§8/§9/§10.
> Design: `docs/specs/2026-06-08-p2-hub-ble-design.md`. Stop point: firmware compiles clean
> (`pio run -e beacon`) + host tests pass (`pio test -e native`); the hub builds in Xcode; **on-device
> bonded-link acceptance + heap re-measure are the human tests** (`prd.md` §9).

## Status — running on hardware: AI Usage live over BLE + device-direct intact; buddy round-trip + P2-0 remain

Spec + plan reviewed by two independent passes (Codex + Claude), all findings verified and folded in
(§2), confirmation re-review **green-lit**. Decisions **owner-confirmed (2026-06-08)**: **D2** retry
once then show error (device "--" + menubar reason); **D3** LESC + bonding, passkey preferred
(P2-A decides passkey-vs-Just-Works on the seamless-UX result); **D4** an Xcode-openable SwiftPM
package (CLI-buildable + Xcode-openable; a hand-generated `.xcodeproj` would not be CLI-verifiable).

**Device (firmware) — DONE, `pio run -e beacon` SUCCESS (RAM 23.6%, Flash 60.1%):**
- `core/hub_proto.{h,cpp}` (Arduino-free codec) + `test_hub_proto` (18 cases). **`pio test -e native`
  74/74 green** (+18, no regressions).
- `core/hublink_ble.{h,cpp}` — NUS GATT peripheral, bonded LESC (passkey, `-DHUBLINK_JUSTWORKS`
  fallback), inbound/outbound stream buffers, callback-enqueue + loop()-dispatch (rev-2/3).
- `core/hub_task.{h,cpp}` — Core-0 wiring: `onFrame` -> `ds_set_usage`/`ds_set_buddy`,
  edge-triggered `ds_set_hub_offline`, running-min heap tracker; `hub_send_permission`.
- 7 buddy views wired (3 callback shapes, send()-gated clear), `ui/pair_overlay.{h,cpp}`,
  `main.cpp` (`hub_task_start`), `lvgl_port.cpp` `-DBEACON_LVGL_PSRAM` flag.
- **Toolchain reality:** the pioarduino esp32s3 libs back the Arduino BLE API with **NimBLE**, not
  Bluedroid (`CONFIG_BT_NIMBLE_ENABLED`). `hublink_ble` uses only the stack-agnostic `BLE*` wrapper,
  so it works on either backing; `HubLink` keeps the screens unaware. `tech.md` §5/§13 updated.

**Hub (`hub/`, SwiftPM) — DONE, `swift build` clean, `swift test` 9/9 green:**
- `BeaconHubKit` — `Protocol.swift` (§7.1 frame encode + command/ack codec), `UsageNormalizer.swift`
  (Claude/Codex -> §7.2). 9 unit tests.
- `beacon-hub` agent — `BeaconCentral` (CoreBluetooth NUS central, OS-mediated pairing, `\n`
  reassembly, MTU-chunked writes, auto-reconnect), `UsagePoller` (Keychain Claude + `~/.codex` Codex),
  `ClaudeCodeBridge` (localhost `NWListener` HTTP, blocking permission hooks -> short-id prompt ->
  ~25 s fail-closed deny, session/statusline), `MenubarController`, `AppDelegate` (frame build +
  heartbeat + reconnect resend), `main.swift` (`.accessory` agent).
- `hub/CONTRACT.md` (frame/command FROZEN; upstream shapes DRAFT pending P2-0), `statusline-shim/`.

### On-hardware bring-up (2026-06-08) — VERIFIED on the Waveshare board + a real Mac

First end-to-end run; these findings + fixes are binding:
1. **Heap re-measure done => PSRAM buffers are now the default.** With LVGL draw buffers in internal
   SRAM, min free internal heap collapsed to ~44 KB under active BLE + WiFi + TLS and **device-direct
   TLS fetches timed out** (weather/markets blank). With **`-DBEACON_LVGL_PSRAM`** (added to
   `platformio.ini` `build_flags`, not just opt-in): boot ~253 KB, steady ~115 KB, transient min
   ~53 KB, and **both planes work** — AI Usage live over BLE AND weather/markets live over WiFi+TLS,
   simultaneously. `tech.md` §2 updated with the numbers. (The ~53 KB transient sits just under the
   60 KB guideline but is stable; revisit only if it tightens.)
2. **Hub RX writes must be acknowledged (`.withResponse`).** `.withoutResponse` packets are silently
   dropped under WiFi+BLE congestion, corrupting multi-chunk status frames (device logged
   `hub: bad/ignored frame`). `BeaconCentral.send` now uses `.withResponse` — reliable + ordered; the
   data rate is tiny so the extra round-trips are free.
3. **The hub must run as a signed `.app` bundle launched via LaunchServices.** macOS TCC only reads
   `NSBluetoothAlwaysUsageDescription` for a LaunchServices-launched, code-signed app — a bare
   `swift run` (or running the binary by path) is **aborted as a privacy violation**. Added
   `Info.plist` + `build-app.sh` (ad-hoc sign + `open`); run via `./build-app.sh run`.
4. **Keychain token is cached (one prompt).** Reading `Claude Code-credentials` per poll re-prompted
   every 45 s; `ClaudeUsageProvider` now caches the token (re-reads only on 401). "Always Allow"
   persists per build (ad-hoc signing changes identity each rebuild => one prompt after each rebuild).
5. **Confirmed at runtime:** MTU negotiates to 247; bonding `OK (bonded)`; NimBLE host task healthy;
   NTP+RTC time; `up=1 time=1`. **Codex usage relays to the device live.**
6. **Claude `oauth/usage` 429s => Claude usage now comes from the statusline `rate_limits`.** The
   unofficial oauth endpoint returns HTTP 429 (Anthropic's subscription-limits change). Claude Code's
   statusline JSON carries `rate_limits.five_hour/seven_day` (first-party, no token), so the hub reads
   Claude 5h/7d from there: `ClaudeCodeBridge` emits `onClaudeUsage`, `AppDelegate` prefers it over the
   poller (oauth kept as best-effort fallback + a `User-Agent` header). Codex unchanged. `tech.md`
   §7.2/§7.3 + spec §4.1/§4.3 + `CONTRACT.md` §C.1/§C.4 updated.
7. **Claude Code wiring (native http hooks + fixed port).** `ClaudeCodeBridge` binds a fixed
   `127.0.0.1:8765` so Claude Code's native `http` hooks use a static URL. `PreToolUse` (scoped, e.g.
   `Bash`) drives approve/deny; session hooks drive idle. The **statusline shim wraps the user's
   existing renderer** (forwards JSON to the hub, then delegates) so the status bar is unchanged.
   Config: `hub/claude-code-settings.snippet.json`; `~/.claude/settings.json` `statusLine` points at the
   wrapping shim. Permission response is `hookSpecificOutput.permissionDecision` (`timeout` in seconds).

**Remaining (human-gated):**
- **Buddy permission round-trip acceptance** (`prd.md` §9 / FR-BUDDY-2/3): with the `PreToolUse` http
  hook configured, confirm a real tool prompt approves/denies on the device within the hook timeout,
  and kill-hub => `ST_HUB_OFFLINE`. (Usage path - Codex live, Claude via statusline - is accepted; the
  permission round-trip is the last unverified MUST.)
- **P2-0** capture — replace the DRAFT upstream shapes in `hub/CONTRACT.md` with real redacted
  captures; settles the `TODO(P2-0)` stubs (Claude/Codex 401 refresh, Codex local fallback, exact
  hook/statusline field names + permission-hook response shape).
- **P2-F (optional):** menubar polish (per-provider mirror, pairing/forget-bond UX).

## 0. BLUF

P0 froze the `HubLink` interface + the `usage_rec_t`/`buddy_rec_t` records + the §7 BLE frame schema;
P1 proved the device-direct plane. **The AI Usage + Coding Buddy screens (14 views) already render
from the DataStore** with full state handling — they run on `dev_seed` today and the buddy buttons
hit a stub. P2 builds the **hub plane** that feeds them real data:

1. **Device:** a Bluedroid `HubLink` impl (NUS peripheral, bonded LESC, `\n`-framed JSON) + a Core-0
   wiring task that parses inbound frames into `ds_set_usage`/`ds_set_buddy`, flips to
   `ST_HUB_OFFLINE` on disconnect, and sends Approve/Deny via `HubLink::send`.
2. **Hub (`hub/`, Swift):** a macOS menubar app that polls Claude+Codex usage (tokens stay on the
   Mac), ingests Claude Code hooks for buddy state + permission prompts, and bridges both to the
   device over BLE — with auto-reconnect and a clear connection status.
3. **Proof:** re-measure the >= 60 KB internal-heap floor under an active bonded link + cert TLS +
   LVGL (closes the coexistence caveat `tech.md` §2/§13), and record fixtures in `hub/CONTRACT.md`.

The DataStore, screen records, and §7 protocol **do not change shape** — P2 writes into the frozen
setters (`ds_set_usage`, `ds_set_buddy`, `ds_set_hub_offline`) and `HubLink::send`. The only
screen-side edits are in the 7 buddy views' existing decide paths (3 distinct callback shapes, §2.1).

## 1. What exists vs. what P2 adds

| Layer | Exists (P0/P1) | P2 adds |
|---|---|---|
| Contracts | `core/hublink.h` (iface), `records.h` (`usage_rec_t`/`buddy_rec_t`), `datastore.{h,cpp}` setters incl. `ds_set_hub_offline` | (none — build against them) |
| Device BLE | none (P0 spike was advertising-only, separate sketch) | `core/hub_proto.{h,cpp}` (codec), `core/hublink_ble.{h,cpp}` (Bluedroid), `core/hub_task.{h,cpp}` (wiring) |
| Device screens | 7 usage + 7 buddy views render records + state; buddy decide paths are stubs (3 shapes) | 7 bespoke decide-path edits => `hub_send_permission`; a Settings pair overlay |
| Hub | none | `hub/` Xcode app: `UsagePoller`, `ClaudeCodeBridge`, `BeaconCentral`, `StatusFrameBuilder`, menubar |
| Fixtures | none | `hub/CONTRACT.md` recorded usage + hook payloads (captured P2-0) |
| Proof | spike: 160 KB free under advertising + insecure TLS | heap re-measure under bonded link + cert TLS + LVGL |

## 2. Review-driven revisions (binding — each verified against the tree)

Two independent reviews (Codex + Claude agent); findings verified at the cited file:line, valid ones
made binding here (these supersede any looser text):

1. **The 7 buddy views use 3 callback shapes, NOT one stub.** Verified: `decide_cb`+user-data in
   editorial/blueprint/analog/oscilloscope, `decide_clear`+`on_*` in calm, `decide(bool)`+`*_cb` in
   led/hud; only `buddy_editorial.cpp:13` has the "hub round-trip is P2" string. So the screen wiring
   is **7 bespoke edits across 3 shapes** calling a shared `hub_send_permission` helper — not a
   find-replace. (Codex 6.1 / Claude M1.)
2. **Inbound frames must NOT be handled in the BLE callback.** `hublink.h:9` freezes `onFrame` as
   loop()-context; Bluedroid RX callbacks run in the BT stack task. The RX callback **only copies
   bytes into a queue + returns**; `loop()` (Core-0 `hub_task`) reassembles on `\n`, parses, and writes
   the DataStore. (Codex 1.1/2.1 BLOCKER.)
3. **`send()` is Core-1-safe by enqueue.** The buddy decide path runs on Core-1 (LVGL); `send()`
   copies the frame into a queue and returns (`hublink.h:19-23`), and the Core-0 `hub_task` notifies.
   No `esp_ble_*` on Core-1.
4. **Clear the prompt only on `send() == true`.** `send()` returns false when disconnected/queue-full;
   an unconditional optimistic clear hides an unsent decision. Gate the local clear on the enqueue
   result; otherwise leave the prompt visible (the hub's next frame is authoritative). (Codex 1.5.)
5. **Stamp `last_updated = now_s()` at frame receipt — but the hub must assert freshness.** Re-stamping
   on every heartbeat would keep stale usage `ST_LIVE` forever if the hub's poller broke. Fix is
   hub-side: the `usage` block carries the **last successful poll**; a failing/old provider is sent as
   `null` windows (live-but-unavailable, "--"), never a re-asserted stale number. Device `ST_STALE`
   then catches the distinct "frames stopped while connected" case. (Codex 1.2/1.3.)
6. **Hook ids exceed `BUDDY_ID_LEN` (24).** `records.h:10` => id holds <= 23 chars; CC request ids can
   be longer. The **hub** mints a short BLE-safe id mapped to the full hook id; the device echoes the
   short id. Documented in `hub/CONTRACT.md`. (Codex 1.4.)
7. **Heap re-measure = running min + a real force-PSRAM flag.** `fetch_task.cpp:75` logs heap every
   ~10 s (a snapshot that misses troughs) — track a **running min sampled every iteration** instead.
   `BEACON_LVGL_PSRAM` **does not exist**, and `lvgl_port.cpp:47-57` auto-falls-back only at boot
   (before BLE), so it can't react to active-link load. P2 **adds** a real `-DBEACON_LVGL_PSRAM` flag
   forcing PSRAM buffers; the re-measure is a **two-build comparison** (default vs forced-PSRAM).
   (Codex 5.1/5.2.)
8. **macOS pairing is OS-mediated.** `CBCentralManager` does not surface passkey entry in app code;
   accessing an encrypted characteristic triggers the macOS system pairing dialog. The device shows
   the passkey (DisplayOnly); the Mac shows the OS prompt. The **device pair overlay is a P2-A
   deliverable** (prerequisite for any bonding test), not P2-F polish. (Codex 3.1/4.1.)
9. **Fixtures captured at P2-0, before P2-A.** `tech.md` §7.3 mandates `hub/CONTRACT.md` *at P2 start*
   so both sides test the **same real** field names (unofficial endpoints/hooks churn). The device
   codec + its host tests build against real captures from the start, not guesses back-filled later.
   This also settles D2 (refresh field names) and the `PreToolUse` vs `PermissionRequest` question.
   (Claude M3 / Codex 4.2/4.3.)
10. **Permission timing: target < 5 s, fail-closed cap ~25 s.** `tech.md` §8 / FR-BUDDY-3: the design
    target is < 5 s; ~25 s is only the ceiling (kept below CC's ~30 s hook timeout). The device runs
    no permission timer — the hub owns the authoritative timeout. (Claude M2.)
11. **Single-prompt contract => hub serializes.** `buddy_prompt_t` holds one prompt (`records.h:59-64`);
    the hub queues a second concurrent permission FIFO, or auto-denies+labels it to avoid stacking two
    ~25 s holds (default; policy documented in `hub/CONTRACT.md`). (Claude m5.)
12. **`ST_RECONNECTING` is unused in P2.** The views guard on it but P2 produces no such transition;
    device goes `ST_HUB_OFFLINE -> ST_LIVE` directly. State it, so reviewers don't expect a dead path.
    (Claude m6.)
13. **Log id + decision only, never the command hint.** The `hint` carries the real command; `tech.md`
    §9 mandates logging id+decision. Hub must not log the hint or token. (Claude m7.)
14. **Codec is the unit-test surface; the BLE stack is integration-tested on hardware** (nRF Connect,
    then the hub). `hub_proto.cpp` is Arduino-free (only `<ArduinoJson.h>`+`records.h`+libc) and joins
    `[env:native]` `build_src_filter` (currently lacks it, `platformio.ini`). Mirrors the P1 split.
15. **Citation hygiene:** P2-A implements `tech.md` §7.1 (BLE schema) + §7.0 (the iface); it does NOT
    "implement FR-STATE-0" (that froze the contracts in **P0**, already done) — P2 builds against it.
    (Claude m1/m2.)

## 3. Chunks (each independently buildable; build after every chunk)

Order = capture the real contract first, then de-risk the unproven BLE-under-load, then layer hub
features. **P2-0 gates P2-A.**

### P2-0 — Capture the real contract  (`tech.md` §7.3 fixtures-at-start)
- On the owner's Mac, capture **redacted** real payloads into `hub/CONTRACT.md`: Claude `oauth/usage`,
  Codex `wham/usage` (+ the auth-token + refresh field names/endpoint for D2); a `PreToolUse` and a
  `PermissionRequest` hook body (confirm whether they are aliases); a `SessionStart`/`Stop`/
  `Notification` body; a `statusline` body. Derive the canonical §7.1 `status_frame.ndjson` +
  `commands.ndjson` and the short-id<->hook-id mapping.
- **Human step** (needs the owner's machine + Claude Code/Codex). Output: `hub/CONTRACT.md`.

### P2-A — Device protocol codec + Bluedroid HubLink + pair overlay  (`tech.md` §7.0/§7.1, FR-HUB-3)
- **`core/hub_proto.{h,cpp}`** (Arduino-free): `hub_reassembler` (split on `\n`, cap+drop overflow);
  `hub_parse_status` (`v==1` gate, null=>`pct=-1`, prompt present/absent => idle, truncate to `*_LEN`);
  `hub_build_permission(buf,id,approve)` (newline-terminated, id-echo).
  Add to `[env:native]` `build_src_filter`. Built against the **P2-0** fixtures.
- **`core/hublink_ble.{h,cpp}`** (device-only): `class HubLinkBle : public HubLink`. Bluedroid GATT
  server, NUS UUIDs (`tech.md` §7.1), advertise `Beacon-<chipid>`; LESC + bonding + allowlist, IO cap
  DisplayOnly. **RX-write callback: copy bytes into an inbound queue + return only** (rev-2). `send()`
  => outbound queue (rev-3). `loop()` drains inbound (reassemble + `hub_frame_cb`) + outbound (notify
  in `mtu-3` chunks). `isConnected()` from the GATT conn state.
- **Device pair overlay (rev-8):** a Settings "Pair" affordance that puts the link in pairing mode and
  shows the 6-digit passkey (DisplayOnly). Prerequisite for the bonding test.
- Build: `pio run -e beacon`. Host test: `test/test_hub_proto` (table-driven over P2-0 fixtures).
- **Device check (GATT/serial level — no DataStore wiring yet):** from **nRF Connect**, confirm
  encrypted RX write is **rejected before bonding** and accepted after; passkey shows on device +
  macOS/nRF prompts; TX subscribe + notify chunking; allowlist; reconnect. Serial-log each reassembled
  frame + each built command. (The "usage screen updates / Approve notified" *visual* check is P2-B,
  once the wiring lands — rev-1/Codex 6.1.)

### P2-B — Device wiring + screen send + heap re-measure  (FR-USAGE-2/3, FR-BUDDY-2/3, FR-STATE-1)
- **`core/hub_task.{h,cpp}`** (Core-0): owns `HubLinkBle`; `begin()` + registers `onFrame`. `onFrame`
  (loop context) => `hub_parse_status` => `ds_set_usage`/`ds_set_buddy` (stamp `last_updated=now_s()`);
  edge-triggered `ds_set_hub_offline()` on the up=>down edge; pump `loop()` ~50 Hz; **running-min
  `int_free`** tracker (rev-7).
- **`main.cpp`:** start `hub_task` alongside `fetch_task` (both Core-0); `BEACON_DEV` keeps `dev_seed`
  for the hub plane.
- **Buddy decide paths (7 views, 3 shapes — rev-1):** add `hub_send_permission(id, approve)` in
  `hub_task`; edit each view's existing decide path to call it and **clear `prompt.present` only if
  `send()` returned true** (rev-4); keep the hub-offline/reconnecting guard.
- **Heap re-measure (spec §7):** running-min under active link + TLS fetch + heaviest screen; add the
  real `-DBEACON_LVGL_PSRAM` flag to `lvgl_port`; two-build comparison; record the number.
- Build: `pio run -e beacon`; `pio test -e native` green. **Human test:** the floor holds (or
  forced-PSRAM build); nRF-Connect-driven frame updates the four windows + idle state + approve.

### P2-C — Hub skeleton: menubar + BLE central  (FR-HUB-3)
- **`hub/` Xcode project** (D4): SwiftUI `MenuBarExtra` agent (`LSUIElement`), macOS 13+.
- **`BeaconCentral`** (`CBCentralManager`): scan (name prefix + service UUID), connect, **trigger
  encryption => OS-mediated pairing dialog** (rev-8; not app-handled), discover NUS, subscribe TX
  notify, write RX; auto-reconnect; reassemble inbound acks on `\n`.
- **`StatusFrameBuilder`:** serialize a `usage`+`buddy` model => NDJSON, chunk to MTU, send on change
  + heartbeat + **full resend on (re)connect**.
- **Menubar:** scanning / connected `<name>` / disconnected + last-sync age + Pair + Quit.
- **Prove:** push a hardcoded usage+buddy frame => the device's four windows + idle state go live;
  killing the app => `ST_HUB_OFFLINE`; relaunch => auto-reconnect => live; macOS bond persists.
- Build: Xcode. Swift unit test: frame builder (model => canonical NDJSON).

### P2-D — Hub usage pollers  (FR-USAGE-1, FR-HUB-1, `tech.md` §7.2/§9)
- **`UsagePoller`** behind a `UsageProvider` protocol (isolate unofficial endpoints):
  - **Claude:** Keychain token (`Claude Code-credentials`) => `oauth/usage` => `five_hour`/`seven_day`
    => `h5`/`d7` (`utilization`->`pct`, ISO `resets_at`->epoch).
  - **Codex (D1):** `~/.codex/auth.json` => `wham/usage` => `primary`/`secondary`; local
    `rollout-*.jsonl` fallback.
  - **Normalize** to §7.2 (`pct` int|null, `reset` epoch); token-refresh per **D2** (field names from
    P2-0); the `usage` block carries the **last successful poll**, `null` on prolonged failure
    (rev-5); never log token contents.
- Poll 30–60 s => feed `StatusFrameBuilder`.
- **Human test:** the device shows real Claude+Codex 5h/7d values that track the Mac within a minute.
- Swift unit test: normalizers, table-driven over the P2-0 fixtures.

### P2-E — Hub Claude Code bridge: prompts + idle state  (FR-BUDDY-1/2/3/5, FR-HUB-2, `tech.md` §7.3)
- **`ClaudeCodeBridge`** — local HTTP server on `127.0.0.1:<port>`:
  - **Permission hook (blocking, `PreToolUse` + `PermissionRequest`):** mint a short id (map to the
    full hook id, rev-6), publish `prompt{id,tool,hint}`, hold the response until the device decides or
    the **~25 s fail-closed cap** (target < 5 s, rev-10) => `permissionDecision`; cap => `deny` + label.
  - **Concurrent prompts:** serialize FIFO, or auto-deny+label the second (rev-11; policy in CONTRACT).
  - **Session/idle:** `SessionStart`/`Stop`/`Notification` hooks => running/waiting + entries.
  - **`statusline` shim (D6):** a small script the owner sets as `statusLine`; POSTs tokens/context.
  - **Logging:** id + decision + timestamp only — **never the hint/command or token** (rev-13).
- Wire decisions back from `BeaconCentral` RX => resolve the held hook response.
- **FR-BUDDY-5 acceptance check:** confirm no buddy view exposes multiple-choice / "don't ask again" /
  text entry — only Approve/Deny + idle (rev / Claude m3).
- **Human test:** a real CC tool prompt appears on the device; Approve/Deny resolves it within the
  hook timeout; idle state reflects live counts.

### P2-F — Polish  (optional last)
- Menubar polish: per-provider usage mirror, pairing/forget-bond UX.
- Finalize `hub/CONTRACT.md`; update `tech.md` §2 (heap number), README P2 box, `prd.md` status.

## 4. New / touched files

```
DEVICE
src/core/hub_proto.{h,cpp}     new (Arduino-free codec; + build_src_filter for native)
src/core/hublink_ble.{h,cpp}   new (Bluedroid HubLink impl; inbound+outbound queues)
src/core/hub_task.{h,cpp}      new (Core-0 wiring + hub_send_permission helper + running-min heap)
src/ui/screens/views/buddy_*.cpp (x7)  edit (3 decide-callback shapes -> hub_send_permission; send()-gated clear)
src/ui/screens/views/settings_*.cpp    edit (Pair overlay affordance; minimal)
src/ui/lvgl_port.cpp           edit (honor -DBEACON_LVGL_PSRAM force flag)
src/main.cpp                   edit (start hub_task; BEACON_DEV gate)
platformio.ini                 edit (native build_src_filter += hub_proto; BEACON_LVGL_PSRAM flag)
test/test_hub_proto            new (reassembly/parse/build, table-driven over P2-0 fixtures)

HUB (new tree)
hub/Beacon.xcodeproj           new (D4 confirmed: Xcode .app bundle, MenuBarExtra agent)
hub/Sources/{UsagePoller,ClaudeCodeBridge,BeaconCentral,StatusFrameBuilder,Menubar,...}.swift  new
hub/Sources/statusline-shim/   new (D6 script)
hub/Tests/                     new (normalizer + frame-builder unit tests)
hub/CONTRACT.md                new (P2-0; recorded, redacted fixtures + short-id<->hook-id map)

DOCS
tech.md                        edit (§2 heap number once measured)
prd.md, README.md              edit (P2 status / roadmap box on hardware acceptance)
```

## 5. Conventions / guard rails (`tech.md` §10)
- **No BLE on Core-1.** All `esp_ble_*` + notify on Core-0 (`hub_task`); BLE callbacks copy+enqueue+
  return only; `onFrame`/parse/DataStore writes run from `loop()`. UI reads DataStore snapshots.
- **Honest state:** every path maps to a `screen_state_t` via the frozen setters; disconnect =>
  `ST_HUB_OFFLINE` (last values + age); failing poll => `null` windows ("--"), never a fake live
  number. Permission **fails closed** (deny), hub-side timeout authoritative.
- **Secrets stay on the Mac.** Only `pct`/`reset` + buddy text cross BLE; never log tokens **or the
  command hint**; fixtures redacted; no secrets in the repo. BLE bonded + LESC-encrypted + allowlist;
  hub HTTP binds localhost.
- **One epoch:** stamp hub `last_updated = now_s()` so staleness ages on the same clock as P1.
- ASCII only (`=>`), surgical diffs, no hardcoded theme colors/fonts in the buddy views, table-driven
  host tests. Swift: no force-unwraps outside tests; explicit Keychain/BLE error handling.

## 6. Risks / decisions
- **Coexistence under load is the headline risk** (`tech.md` §2/§13): bonded link + cert TLS + LVGL
  was never measured (the spike was advertising + insecure TLS only). P2-B gates on the >= 60 KB
  floor; the new `-DBEACON_LVGL_PSRAM` build is the escape valve (auto-fallback fires only at boot, so
  it cannot react to active-link load — rev-7).
- **BLE bonding/passkey on Bluedroid core 3.3.x + OS-mediated macOS pairing (D3, confirmed):** LESC +
  bonding is mandatory; passkey is preferred but the device-DisplayOnly + Mac-passkey UX is unproven.
  P2-A proves it on hardware (nRF Connect, then macOS) before any Swift and **decides passkey vs
  Just-Works on the seamless-UX result** (security-first, but Just-Works bonding - still encrypted -
  wins if passkey is clunky).
- **Unofficial usage endpoints** (Claude `oauth/usage`, Codex `wham/usage`): isolate behind
  `UsageProvider`; on failure => `null` windows + the screen ages naturally, not a crash. D1 local
  fallback for Codex.
- **OAuth token expiry (D2, confirmed):** retry once on 401 (refresh field names captured in P2-0);
  on failure emit `null` windows ("--" on device) + the actionable error reason in the menubar.
- **Blocking hook round-trip:** the hub holds the permission hook open up to ~25 s (target < 5 s);
  ensure the CC hook-timeout config (~30 s) leaves margin; cap => deny + label (FR-BUDDY-3).
- **Swift build (D4, confirmed):** Xcode project (owner has Xcode); SwiftPM was the CI-friendlier alt.
- **`PreToolUse` vs `PermissionRequest`:** handle both names until P2-0 confirms whether they alias.

## 7. Done = stop-for-human-test
`pio run -e beacon` builds clean with `hub_task` active, `pio test -e native` green, the hub builds
and runs in Xcode. The **human tests** (the `prd.md` §9 hardware acceptance, checked off only after
they pass): bond once + auto-reconnect after a hub restart; the four usage windows track the Mac
within a minute; a real Claude Code prompt approves/denies on the device within the hook timeout;
killing the hub flips to `ST_HUB_OFFLINE`; and the **>= 60 KB internal-heap floor holds** under the
active bonded link + cert TLS + LVGL (the number recorded here and in `tech.md` §2).
