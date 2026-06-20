# Device -> Hub Ticker Report (fresh-hub adoption on pairing)

- **Issue:** #105
- **Status:** design
- **Builds on:** #92 (hub -> device ticker config, `docs/specs/2026-06-17-hub-ticker-config-design.md`)
- **Contract:** additive `cmd:"report"` frame on the device -> hub channel; proposed `hub/CONTRACT.md` §B3 (added with this work)

## 1. Problem

The ticker protocol (#92) is **push-only, hub -> device**. The device persists the configured list
to NVS and live-applies it, but has no frame to report that list back.

Symptom: configure tickers on the device from Mac A, then pair the device with a *fresh* hub on Mac B.
Mac B has no `UserDefaults`, so `TickerConfigStore` initializes empty (`rev == 0`, `rows == []`). The
hub's `pushTickerConfig()` early-returns on an empty list (`ConfigFrame.chunks` returns `[]`), so the
hub never learns what the device already holds. The Tickers panel shows an empty "Current list"
(0/16) while the device keeps running its NVS list.

## 2. Goal

On pairing, a fresh (never-configured) hub **adopts** the device's current ticker list and shows it in
the panel, instead of starting blank.

## 3. Non-goals

- Two-way merge / conflict resolution when both sides are non-empty (hub stays authoritative).
- Re-adopting after the user has explicitly emptied the hub list (`rev > 0`).
- Per-device differing lists.
- Persisting the hub's `rev` on the device (NVS schema stays frozen; see §6).

## 4. Approach

Add a device -> hub **`report`** frame carrying the device's current running rows (full rows, chunked,
mirroring the `config` row schema). The device emits it **once per connection** after the first inbound
hub frame. A **pristine** hub (`rev == 0 && rows.isEmpty`) adopts it; any other hub ignores it and
stays the source of truth.

```
Mac B (fresh hub)                          Device (holds NVS list from Mac A)
-----------------                          ----------------------------------
connect + subscribe
onReady: sendFullFrame()  ----status--->   on_frame (FIRST inbound this conn)
         pushTickerConfig() [no-op,                |
         store empty => chunks() == []]            | s_reported == false => emit
                                                    v
                          <---cmd:report (chunked, full rows)---
DeviceCommand.report (reassemble)
store pristine? yes => adopt:
  save(rows)            rev 0 -> 1
  refresh panel
  setTickerSync(.synced)
  (NO push back -- device already has it)
```

On a **non-fresh** reconnect the hub still receives a report (the device emits unconditionally), but the
hub ignores it because its store is non-pristine; the hub's existing `onReady` `pushTickerConfig()`
reconciles the device as before. The redundant report is harmless.

One reconcile gap is pre-existing and **out of scope**: if the user emptied the hub list (`rev > 0`,
`rows == []`), `pushTickerConfig()` early-returns on the empty list *and* the firmware rejects an empty
assembled `config` (`hub_config_accum_step` => `empty`). So the device keeps running its previous list;
the protocol has no "clear to empty" push. #105 does not change this — it is the same divergence that
exists today, and "emptying the device list" is not a goal here.

The device **always** reports its current running table, including the seeded defaults of a
never-configured device (issue #105 Q2-A). Adopting defaults yields an agreed, correct state with no
added device-side provenance flag; a fresh hub would otherwise push those same defaults anyway.

## 5. Contract (additive) — `hub/CONTRACT.md` §B3

The `report` rides the existing **device -> hub `cmd` channel** (alongside `permission` and
`config_ack`). The frozen `config` / `config_ack` / status / buddy / loc / permission blocks are
untouched.

### Device -> hub: ticker report (chunked snapshot of the current running list)

```json
{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":2,"tickers":[
  {"id":"ygspc","src":"yahoo","sym":"%5EGSPC","name":"S&P 500","kind":"index","cadence":300,"stale":600,"basis":"prev_close"}]}
{"v":1,"cmd":"report","what":"tickers","rev":0,"part":1,"parts":2,"tickers":[
  {"id":"bbtcusdt","src":"binance","sym":"BTCUSDT","name":"BTC","kind":"crypto","cadence":60,"stale":600,"basis":"24h"}]}
```

- `cmd` — `"report"` (new device -> hub command verb).
- `what` — `"tickers"` (namespaces the report so a future report kind can reuse the verb without an
  ambiguous parse). Hub ignores any other `what`.
- `rev` — **always `0`** (issue #105 Q1-A). The device does not persist the hub's revision, and the
  hub never uses the reported value for the adopt decision (it adopts its own pristine `rev 0 -> 1`).
  Carried only for structural symmetry with `config` and chunk-continuity validation.
- `part` / `parts` — 0-based chunk index and total; rows concatenated across parts in `part` order ==
  display order, exactly like `config`.
- `tickers` — full rows, **same key schema and caps as `config`**: `id` (<=15), `src`
  (`binance`|`yahoo`), `sym` (<=23, Yahoo percent-encoded once; Binance raw), `name` (<=23), `kind`
  (`fx`|`crypto`|`index`|`etf`), `cadence` (int s), `stale` (int s), `basis` (`prev_close`|`24h`).
- **Chunking rule:** same byte budget and framing as `config` — each newline-terminated line `<= ~900 B`
  (margin under firmware `HUB_FRAME_MAX` = 1024); a row is **never split**; trailing `0x0A`. The row
  count is `<= MAX_TICKERS` (16) because the **device `ticker_table` is bounded to 16**, not because the
  chunking encoder enforces a cap (the hub's `ConfigFrame.chunks` does not; only the firmware `config`
  parser rejects an over-16 assembled snapshot).

**No ack.** The report is one-way and informational. The hub adopts or silently ignores it; there is
no new device-facing ack frame, and the device does not wait for one.

**Backward compatibility:** an older hub that does not know `cmd:"report"` parses it as an unknown
command and drops it (`DeviceCommand.parse` returns `nil` for an unrecognized `cmd`). No behavior
change on old hubs.

## 6. Device side (firmware)

### 6.1 Row serializer — `core/hub_proto.cpp`

The device today only **parses** rows (`hub_parse_config_chunk`). Add the inverse, split into a pure
**plan** (chunk boundaries — cheap, just indices) and a pure **single-frame** serializer:

```c
// Greedy whole-row chunk planner. Fills group_start[g] with the first-row index of chunk g and returns
// the chunk count (== parts), 1..MAX_TICKERS. Returns 0 on any failure (count < 1 or > MAX_TICKERS, an
// unmappable enum, or a single row that alone exceeds the budget). All-or-nothing: a 0 means emit nothing.
int hub_report_plan(const ticker_runtime_t* rows, int count, int group_start[MAX_TICKERS]);

// Serialize rows[lo..hi) as one newline-terminated cmd:"report" frame (part `part` of `parts`) into buf.
// Returns bytes (incl. '\n', excl. NUL), or 0 on overflow / unmappable enum.
size_t hub_build_report_frame(const ticker_runtime_t* rows, int lo, int hi,
                              int part, int parts, char* buf, size_t cap);
```

Why a plan/frame split instead of one "write all chunks" call: materializing up to `MAX_TICKERS`
frames at once is `16 * HUB_FRAME_MAX` = 16 KB, which overflows `hub_task`'s 8 KB stack and wastes
scarce internal RAM. The caller (§6.2) plans once, then serializes+sends one `HUB_FRAME_MAX` (1 KB)
frame at a time.

- Maps the runtime enums back to wire strings — the inverse of `map_source` / `map_kind` /
  `map_basis` (new `src_to_wire` / `kind_to_wire` / `basis_to_wire` helpers).
- `hub_report_plan` greedy whole-row packing mirrors the hub's `groupRows`: extend the current chunk
  while the serialized line stays `<= ~900 B`, else start a new chunk. It measures each candidate group
  with `parts = count` (worst-case header digits; real `parts <= count`, so a planned group still fits
  when re-serialized with the real `parts`) and validates enums up front, so if `plan` succeeds every
  `hub_build_report_frame` call also succeeds (preserving all-or-zero at the device).
- `rev` field hard-coded to `0`.
- A row count of `0` cannot occur — `ticker_table` always holds `>= 1` row (defaults seed when the NVS
  blob is absent). `hub_report_plan` still guards `count >= 1` and returns `0` otherwise.

### 6.2 Emit gate — `core/hub_task.cpp`

```c
static bool s_reported = false;   // emitted the ticker report this connection?
```

- In `on_frame` (Core-0, single inbound handler): gate at the **top**, before the dispatch logic — if
  `!s_reported`, snapshot the live table, `hub_report_plan` it, then serialize+send each chunk one at a
  time (single `HUB_FRAME_MAX` buffer) via `g_link->send`, set `s_reported = true`, then fall through to
  the normal frame dispatch. Placing it at the top (not after
  dispatch) is deliberate: `on_frame` has several early `return`s (ack at `:92`, config at `:97`), so a
  bottom-of-function emit would be skipped whenever the first inbound frame is an ack/config. Emit order
  relative to processing the inbound frame is irrelevant (the report is an independent device -> hub
  send). This is the "after the first inbound hub frame" gate — the same proof-of-subscription signal
  that `config_ack` / `permission` rely on.
- Reset `s_reported = false` on the **disconnect edge** already detected in the task loop
  (`hub_task.cpp:135`, where `s_was_connected && !c`). Next connection re-reports.
- Reads the table via a snapshot under the existing `ticker_table` lock (`ticker_table_get` /
  `ticker_table_count`); no new threading.
- Native host: `g_link == nullptr`, so the emit no-ops (consistent with `send_config_ack`).

Emit ordering note: on the fresh-pairing path the hub **never pushes a `config`** — its empty store
makes `pushTickerConfig()` a no-op — so there is no config/report cross-talk regardless of which inbound
frame (full status, usage, buddy, or loc) trips the gate first. (The hub has several status-frame send
sites; the design does not rely on the first inbound being the full frame, only on no config being
pushed.) On a non-fresh reconnect the device may emit the report before/after the inbound `config`; both
sides converge (device applies config; hub ignores the report). Order does not matter.

## 7. Hub side (Swift)

### 7.1 Parse — `BeaconHubKit/Protocol.swift`

Extend `DeviceCommand` with a per-chunk case:

```swift
case report(what: String, rev: UInt32, part: Int, parts: Int, rows: [TickerRow])
```

`DeviceCommand.parse` gains a `case "report":` that validates `what == "tickers"`, `part`/`parts`
bounds, and decodes `tickers` into `[TickerRow]` (reusing the `config` row decode + caps). Any
malformed chunk returns `nil` (dropped). Unknown `what` returns `nil`.

### 7.2 Reassembly — `BeaconHubKit/ReportAssembler.swift` (pure, host-tested)

A small per-connection accumulator (mirrors the firmware `config_accum_t`, but hub-side). Lives in
`BeaconHubKit` (not `AppDelegate`) so it is host-testable:

- `feed(.report(...))` returns `.assembled([TickerRow])` on the final part, `.pending` mid-stream, or
  `.dropped` on a fault.
- `part == 0` (re)starts accumulation; non-zero parts must be contiguous and share `parts`/`rev`.
- Any gap / duplicate / out-of-range part **discards** the partial (fail-closed) and returns `.dropped`.
- `reset()` clears the partial; `AppDelegate` calls it on the disconnect edge.

### 7.3 Adoption gate — `beacon-hub/AppDelegate.swift` (thin wiring)

`handle(_:)` gains a `.report` case that feeds the assembler and, on `.assembled(rows)`, runs the gate.
`TickerConfigState.isPristine` (`rev == 0 && rows.isEmpty`) is added to `BeaconHubKit` so the decision
is host-testable:

```
case .report(_, _, _, _, let rows):
    switch reportAssembler.feed(chunk) { ... case .assembled(let all): adoptIfPristine(all) }

private func adoptIfPristine(_ rows: [TickerRow]) {
    guard tickerStore.current.isPristine else { return }   // rev == 0 && rows.isEmpty
    tickerStore.save(rows: rows)        // rev 0 -> 1, persists to UserDefaults
    menubar.setTickerRows(tickerStore.current.rows)         // panel now shows the adopted list
    menubar.setTickerSync(.synced(tickerStore.current.rows.count))
    // NOTE: do NOT call pushTickerConfig() -- the device already has this list (avoid an echo loop)
}
```

A non-pristine hub (`rev > 0` or non-empty rows) fails the guard and returns: it stays the source of
truth, and its `onReady` `pushTickerConfig()` continues to reconcile the device.

## 8. Edge cases

| Case | Behavior |
|---|---|
| Fresh hub, device has prior config | Hub adopts; panel fills; `synced`. The fix. |
| Fresh hub, device on seeded defaults | Hub adopts the defaults (Q2-A); agreed state, no push-back. |
| Non-pristine hub reconnects | Hub ignores the report; `onReady` push reconciles the device. |
| User emptied the hub list earlier (`rev > 0`, `rows == []`) | Not pristine (`rev != 0`) => report ignored, no re-adoption (non-goal). Device keeps its prior list — the protocol has no empty-push, a pre-existing divergence #105 does not address. |
| Malformed / incomplete report (bad chunk, gap, drop mid-stream) | Hub discards the partial, stays pristine; next connection re-reports. |
| Old hub (no `report` support) | Drops the unknown `cmd`; no behavior change. |
| Report races inbound `config` on reconnect | Order-independent; both sides converge. |

## 9. Testing

Keep `CONTRACT.md`, `Protocol.swift`, `hub_proto.cpp`, and their tests in sync.

### Firmware (`native` env, Unity)

- New `test_hub_report` suite: `hub_report_plan` + `hub_build_report_frame` — single-chunk frame shape
  (`cmd:"report"`, `what:"tickers"`, `rev:0`); multi-chunk split at the budget with correct `parts` and
  group boundaries; whole-row-only packing (no split rows); enum -> wire mapping for every
  `src`/`kind`/`basis`; `count < 1` / `count > MAX_TICKERS` guard returns 0. Verify each emitted frame
  by deserializing it with ArduinoJson and asserting the `report` envelope + per-row values (the
  `report` envelope differs from `config` — flat `cmd` fields vs a nested `config` object — so
  `hub_parse_config_chunk` is not reused; the device-emit <-> hub-parse agreement is covered by the
  shared §B3 fixture).

### Hub (`BeaconHubKit`)

- `DeviceCommand.parse` — decode a valid `cmd:"report"` chunk; reject bad `what`, out-of-range
  `part`/`parts`, malformed rows.
- Reassembler — in-order multi-chunk assembly; gap/duplicate/out-of-range discard; mid-stream reset.
- Adoption gate — pristine store adopts (`rev 0 -> 1`, panel rows set, `synced`, **no** push); a
  non-pristine store (both `rev > 0` and `rev == 0 && rows non-empty`) ignores.

### Shared fixtures (`CONTRACT.md` §B3)

Record one canonical 2-chunk report payload exercised by both the firmware and hub tests, matching the
existing §B2 fixture discipline.

## 10. Files touched

| File | Change |
|---|---|
| `hub/CONTRACT.md` | Add §B3 (device -> hub `cmd:"report"`). |
| `firmware/src/core/hub_proto.h` / `.cpp` | `hub_report_plan` + `hub_build_report_frame` + enum -> wire helpers. |
| `firmware/src/core/hub_task.cpp` | `s_reported` gate; plan+emit in `on_frame`; reset on disconnect edge. |
| `firmware/test/test_hub_report/` (new) | `hub_report_plan` + `hub_build_report_frame` tests + `test_main.cpp`. |
| `hub/Sources/BeaconHubKit/Protocol.swift` | `DeviceCommand.report` case + parse. |
| `hub/Sources/BeaconHubKit/ReportAssembler.swift` (new) | Pure per-connection chunk reassembler. |
| `hub/Sources/BeaconHubKit/TickerConfigState.swift` | `isPristine` helper. |
| `hub/Sources/beacon-hub/AppDelegate.swift` | Hold a `ReportAssembler`; `handle(.report)` => adoption gate; reset on disconnect. |
| `hub/Tests/BeaconHubKitTests/ProtocolTests.swift` | `DeviceCommand.report` parse tests. |
| `hub/Tests/BeaconHubKitTests/ReportAssemblerTests.swift` (new) | Reassembly + `isPristine`/adoption-decision tests. |
