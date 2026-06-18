# Hub-Driven Ticker Configuration — Hub Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let the user curate the device's ticker list from the macOS menubar — search Binance + Yahoo, build a desired list (≤16), and push it to the device as a chunked BLE `config` snapshot, with ack-driven sync status.

**Architecture:** A pure contract layer in `BeaconHubKit` (canonical `TickerRow`, stable id, exact-once symbol encoding, chunked `ConfigFrame` encoder matching the firmware parser, `config_ack` decode) feeds source adapters (Binance `exchangeInfo` cached + Yahoo live search) and a unified search/merge. The `beacon-hub` executable persists the desired list (+ monotonic `rev`) in `UserDefaults`, pushes on BLE connect + on every edit, and reflects `config_ack` as sync status in the menubar.

**Tech Stack:** Swift 6, SwiftPM (no third-party deps), CoreBluetooth, AppKit `NSStatusItem` + `NSPopover`-hosted SwiftUI, `URLSession`, XCTest. Build: `swift build` / `swift test` / `./build-app.sh run`.

**Scope:** Hub only. Consumes the **frozen firmware contract** from `docs/plans/92-hub-ticker-config-firmware.md` (A4) — must produce byte-exactly what `hub_parse_config_chunk` accepts. Spec: `docs/specs/2026-06-17-hub-ticker-config-design.md` §4. Issue #92.

## Global Constraints

- No third-party dependencies. Swift 6 concurrency; UI types are `@MainActor`.
- The wire format is frozen by firmware A4 — keep `BeaconHubKit` encoding and `hub/CONTRACT.md` in sync. Chunk frames: each serialized line ≤ ~900 B (firmware `HUB_FRAME_MAX`=1024), never split a row across chunks, `part` 0-based, `parts` total.
- Persist source/kind/basis as the wire strings (`binance`/`yahoo`; `fx`/`crypto`/`index`/`etf`; `prev_close`/`24h`).
- `MAX_TICKERS = 16`. Stable id ≤ 15 chars (firmware `FIN_ID_LEN`=16), deterministic from `(src, symbol)` — invariant under reorder/removal.
- Yahoo symbols must be percent-encoded for the URL path EXACTLY ONCE (`^GSPC` => `%5EGSPC`); Binance raw. The device interpolates `sym` straight into the path with no escaping.
- Conventional Commits, scope `hub`. The USER commits; the agent may commit locally on the feature branch (no push) per this session's authorization.
- Match existing patterns: `JSONSerialization` manual parse style for inbound, `JSONEncoder(.sortedKeys)` + `0x0A` framing for outbound, `UserDefaults` for prefs, `NSPopover`+SwiftUI for UI.

---

## Wire contract (frozen by firmware A4 — reproduce exactly)

Hub => device chunk:
```json
{"v":1,"config":{"rev":7,"part":0,"parts":2,"tickers":[
  {"id":"y_gspc","src":"yahoo","sym":"%5EGSPC","name":"S&P 500","kind":"index","cadence":300,"stale":600,"basis":"prev_close"}]}}
```
Device => hub ack (parsed via `DeviceCommand`):
```json
{"v":1,"cmd":"config_ack","rev":7,"ok":true,"count":8}
{"v":1,"cmd":"config_ack","rev":7,"ok":false,"err":"too_many_tickers"}
```

---

## Chunk Roadmap (dependency order)

| Chunk | Deliverable | Mergeable alone? |
|---|---|---|
| **B1** | Contract layer in BeaconHubKit: `TickerRow`, stable id, symbol encoding, chunked `ConfigFrame` encoder, `config_ack` decode | Yes (pure, host-tested) |
| **B2** | Source adapters (Binance cached + Yahoo live) + unified search/merge | Yes (pure mapping host-tested) |
| **B3** | Persistence (`UserDefaults` list + `rev`) + push-on-connect/edit + `config_ack` => sync status | Yes |
| **B4** | Menubar search/editor SwiftUI panel wired to B2/B3 | Yes (completes hub) |

Each chunk ends green: `swift build` + `swift test` pass. Append the §C config spec to `hub/CONTRACT.md` in B1.

---

## Chunk B1: Contract layer (BeaconHubKit)

**Files:**
- Create: `hub/Sources/BeaconHubKit/TickerConfig.swift`
- Modify: `hub/Sources/BeaconHubKit/Protocol.swift` (extend `DeviceCommand` with `config_ack`)
- Modify: `hub/CONTRACT.md` (append §C config frame + config_ack; renumber as needed)
- Test: `hub/Tests/BeaconHubKitTests/TickerConfigTests.swift`

**Interfaces (produces):**
```swift
public enum TickerSource: String, Codable { case binance, yahoo }
public enum TickerKind: String, Codable { case fx, crypto, index, etf }
public enum ChangeBasis: String, Codable { case prevClose = "prev_close", h24 = "24h" }

public struct TickerRow: Codable, Equatable {
    public var id: String          // <= 15 chars, stable, derived
    public var src: TickerSource
    public var sym: String         // wire-ready: Yahoo percent-encoded, Binance raw
    public var name: String
    public var kind: TickerKind
    public var cadence: Int
    public var stale: Int
    public var basis: ChangeBasis
}

public enum TickerID {
    // deterministic, <=15 chars, invariant under reorder: e.g. "<srcPrefix>_<base32(fnv1a(src+":"+sym))>"
    public static func make(src: TickerSource, sym: String) -> String
}

public enum SymbolEncoding {
    public static func yahooPath(_ rawSymbol: String) -> String   // "^GSPC" => "%5EGSPC", encode-once, idempotent-safe
}

public enum ConfigFrame {
    // Split rows into chunks each <= maxBytes serialized (default ~900); never split a row.
    // Returns the ordered newline-terminated frame Datas for this rev. Throws if a single row exceeds maxBytes.
    public static func chunks(rows: [TickerRow], rev: UInt32, maxBytes: Int = 900) throws -> [Data]
}

// extend DeviceCommand:
//   case configAck(rev: UInt32, ok: Bool, count: Int?, err: String?)
//   parse: cmd == "config_ack" => decode rev/ok/count/err
```

**Tasks (TDD):**
- [ ] Test+impl `TickerID.make`: deterministic (same input => same id), ≤15 chars, differs for different (src,sym), invariant (independent of any list).
- [ ] Test+impl `SymbolEncoding.yahooPath`: `^GSPC`=>`%5EGSPC`, `EURUSD=X` unchanged (no `/`, no reserved), a `/` would be encoded; assert NOT double-encoded if already-encoded input is re-run (guard or document single-call contract — prefer: only ever call on raw Yahoo symbols, test the raw cases the firmware expects: `^GSPC`,`^IXIC`,`^DJI`,`GC=F`,`CL=F`,`EURUSD=X`).
- [ ] Test+impl `ConfigFrame.chunks`: a 1-row list => 1 chunk whose JSON matches the exact firmware shape (assert keys `v/config/rev/part/parts/tickers` and a row's keys `id/src/sym/name/kind/cadence/stale/basis`); a 16-row list => multiple chunks, each ≤900 B, `parts` correct, rows concatenated in order, no row split; newline-terminated; `rev` present.
- [ ] Test+impl `DeviceCommand.config_ack` parse: ok-with-count, err variant, ignores unknown.
- [ ] Append §C to CONTRACT.md (wire examples + the err enum + chunking rule).
- [ ] `swift test` green; commit `feat(hub): ticker config contract layer (frame encoder, ack, stable id)`.

---

## Chunk B2: Source adapters + unified search

**Files:**
- Create: `hub/Sources/BeaconHubKit/TickerCatalog.swift` (pure mapping + merge; host-tested)
- Create: `hub/Sources/beacon-hub/TickerSearch.swift` (network fetch using URLSession; thin, calls the pure mappers)
- Test: `hub/Tests/BeaconHubKitTests/TickerCatalogTests.swift`

**Interfaces:**
```swift
public struct TickerCandidate: Equatable { public var row: TickerRow; public var sourceLabel: String } // for UI
public enum BinanceCatalog {
    // map exchangeInfo JSON => candidates (status==TRADING, quote in {USDT,USDC,FDUSD}); USDT first per base
    public static func map(exchangeInfoJSON: Data) -> [TickerCandidate]
    public static func search(_ query: String, in candidates: [TickerCandidate]) -> [TickerCandidate] // local filter on cached set
}
public enum YahooCatalog {
    // map search endpoint JSON => candidates (quoteType => kind; basis=prev_close, cadence=300; sym via SymbolEncoding.yahooPath)
    public static func map(searchJSON: Data) -> [TickerCandidate]
}
public enum TickerMerge {
    // merge Binance(local-filtered) + Yahoo(live) results: keep both sources labeled (duplicates shown);
    // crypto USDT first then USDC/FDUSD; group/sort deterministically for UI
    public static func unify(binance: [TickerCandidate], yahoo: [TickerCandidate]) -> [TickerCandidate]
}
```
Defaults table keyed by (source, kind) supplies cadence/stale/basis (binance crypto: 60/600/24h; yahoo: 300/600/prev_close).

**Tasks (TDD with inlined JSON fixtures, matching the repo's no-fixture-file style):**
- [ ] `BinanceCatalog.map` from a small exchangeInfo fixture => BTC/USDT etc., correct kind/defaults, filters non-TRADING + other quotes, USDT-first ordering; sym raw; id via TickerID.
- [ ] `YahooCatalog.map` from a search fixture => INDEX=>index, ETF=>etf, CRYPTOCURRENCY=>crypto, CURRENCY=>fx, EQUITY=>etf; sym percent-encoded; name from shortname; defaults.
- [ ] `TickerMerge.unify`: a symbol present on both sources yields two labeled candidates; crypto quote ordering; deterministic order.
- [ ] `TickerSearch` (beacon-hub): live Yahoo query (debounced caller-side) + cached Binance exchangeInfo (fetch once, cache to Application Support with a daily TTL); mirror the `URLSession.dataTask` pattern in `UsagePoller`. (Network call itself not unit-tested; the mappers are.)
- [ ] `swift test` green; `swift build` green; commit `feat(hub): Binance + Yahoo ticker source adapters + unified search`.

---

## Chunk B3: Persistence + push + ack wiring

**Files:**
- Create: `hub/Sources/beacon-hub/TickerConfigStore.swift` (UserDefaults-backed desired list + rev)
- Modify: `hub/Sources/beacon-hub/AppDelegate.swift` (push on `onReady`; push on edit; route `config_ack`)
- Modify: `hub/Sources/beacon-hub/HubViewModel.swift` (+ `MenubarController`) — sync status state
- Test: `hub/Tests/BeaconHubKitTests/TickerConfigStoreTests.swift` (the pure rev/list codec; UserDefaults via an injectable store or a `Codable` round-trip)

**Interfaces:**
```swift
struct TickerConfigState: Codable { var rows: [TickerRow]; var rev: UInt32 }
final class TickerConfigStore {            // UserDefaults key "BeaconTickerConfig"
    func load() -> TickerConfigState        // empty list + rev 0 if absent
    func save(rows: [TickerRow])            // bumps rev monotonically, persists
    var current: TickerConfigState { get }
}
// AppDelegate:
//   on central.onReady => push current config (all chunks) after the status frame
//   on edit (from UI) => save(rows) then push
//   on central.onCommand .configAck(rev,ok,count,err) => update HubViewModel.tickerSync
// HubViewModel: @Published var tickerSync: TickerSyncStatus  (.synced(count) / .error(String) / .pending)
```
Pushing: send each `ConfigFrame.chunks` Data via `central.send(...)`, ordered, after the on-connect status frame.

**Tasks (TDD):**
- [ ] `TickerConfigState` round-trip + `save` bumps rev monotonically (host-tested via Codable / injected defaults).
- [ ] Wire `onReady` to push current config (guard: only if list non-empty); wire edit-path push; route `configAck` to `tickerSync`.
- [ ] `swift test` + `swift build` green; commit `feat(hub): persist ticker list + push on connect/edit + ack sync status`.

---

## Chunk B4: Menubar search/editor UI

**Files:**
- Create: `hub/Sources/beacon-hub/TickerEditorView.swift` (SwiftUI)
- Modify: `hub/Sources/beacon-hub/HubPanel.swift` (entry point — a "Tickers" button/module opening the editor) and `MenubarController`/`HubViewModel` wiring
- (No new tests — UI; logic lives in B1/B2/B3 which are tested.)

**Interfaces / behavior:**
- A search field (debounced ~300 ms) driving `TickerSearch` => `TickerMerge.unify`; results grouped/labeled by source (Binance/Yahoo chips), duplicates shown.
- Add appends to the desired list (enforce ≤16, disable add at limit); remove; reorder (drag or up/down).
- Shows `tickerSync` status ("Synced N" / error / pending).
- On any edit, calls the B3 edit-path (save + push).
- Match `HubPanel` styling (Module/Card components, width ~340).

**Tasks:**
- [ ] Build `TickerEditorView` (search + results + current-list editor + sync badge), wire to `TickerSearch`/`TickerConfigStore`/push via the view model.
- [ ] Add the entry point in `HubPanel`.
- [ ] `swift build` green; `./build-app.sh run` smoke (manual). Commit `feat(hub): menubar ticker search/editor panel`.

**Runtime manual verification (B4):** add a few tickers => device shows them live; reorder => order matches; remove => gone; sync badge reflects `config_ack`; >16 disabled.

---

## Self-Review

- **Spec §4 coverage:** adapters (Binance/Yahoo) => B2; unified search + dedup labeling + crypto USDT default => B2; symbol exact-once + stable id => B1; desired-list persistence => B3; push on connect+edit + ack/sync => B3; menubar editor => B4; CONTRACT §C => B1. Frozen wire contract reproduced in B1 and asserted against the firmware row/key shape.
- **Type consistency:** `TickerRow`/`TickerSource`/`TickerKind`/`ChangeBasis` defined in B1, reused B2–B4; `ConfigFrame.chunks` + `DeviceCommand.configAck` defined B1, used B3; `TickerCandidate` B2 used B4.
- **Placeholder scan:** B1 fully stepped; B2–B4 carry exact files + signatures + test intent; per-step code authored against live code at execution (network/UI specifics) — avoids speculative inaccurate Swift.
- **Risk:** Yahoo search endpoint shape/headers (mirror UsagePoller's UA/timeout); the mappers are pure-tested so a shape change is isolated to the thin fetch layer.
