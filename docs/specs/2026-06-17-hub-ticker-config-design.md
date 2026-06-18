# Hub-Driven Ticker Configuration — Design

**Date:** 2026-06-17
**Status:** Draft (brainstormed; Codex design review incorporated; pending user review)
**Scope:** Let the user curate the device's market-ticker list from the macOS hub. Hub pushes the list over BLE; the device persists it (NVS) and live-applies it to the Finance screen. The device keeps fetching prices itself over WiFi.

## Bottom Line

Today tickers are compile-time only (`DEFAULT_TICKERS[]` in `firmware/src/config/tickers.h`); there is no runtime override and the hub has no config-push path. This feature adds:

1. **Contract** — one additive hub=>device `config` frame + one device=>hub `config` ack. Frozen status/buddy/loc blocks are untouched.
2. **Firmware** — a runtime ticker table (owned storage) loaded from NVS-or-default at boot, hot-swappable on push, with live re-apply of fetchers + Finance UI.
3. **Hub** — dynamic browse/search across 2 sources (Binance + Yahoo), a unified menubar search panel, push-on-connect + push-on-edit, and ack-driven sync status.
4. **Frankfurter removal** — drop the Frankfurter source from firmware entirely (lagging daily-ECB, IDR-only; superseded by live Yahoo `=X` FX). See Section 6.

The two-plane architecture is preserved: the hub sends only the ticker *list*; the device still fetches live prices over WiFi. The hub is the source of truth for the desired list; firmware's `DEFAULT_TICKERS` becomes the fallback only (fresh device, never paired, NVS empty/corrupt).

## Decisions (locked during brainstorming)

| # | Decision | Choice |
|---|----------|--------|
| 1 | What the hub sends | Ticker **list/config**, not prices. Device keeps fetching over WiFi. |
| 2 | Hub editing UX | macOS **menubar UI**. |
| 3 | Coverage model | **Dynamic browse/search** across 2 sources (not a static curated catalog). |
| 3b | Frankfurter | **Dropped + cleaned from firmware** (lagging daily-ECB, IDR-only). FX now lives via Yahoo `=X`. |
| 4 | Apply model | **Persist to NVS + live re-apply** (no reboot). |
| 5 | Feedback | Device **acks with result** (ok/err); menubar shows sync status. |
| 6 | Search UX | **Unified search box** — one query, hub merges results from all sources, grouped/labeled by source. |
| 7 | Duplicate tickers across sources | **Show both, labeled by source** (e.g. BTC/USDT - Binance, BTC-USD - Yahoo). |
| 8 | Crypto quote currency | **Default USDT, show top quotes** (USDC, FDUSD) as separate rows. |
| 9 | Sync model | **Full-snapshot replace**, idempotent; push on BLE connect + on every edit; monotonic `rev`. |

## Non-Goals

- Pushing live **prices** from the hub (finance stays on the WiFi/public plane).
- Pushing a **static bundled catalog** (superseded by dynamic browse).
- Per-row user tuning of `cadence_s` / `stale_s` / `change_basis` (derived from a defaults table by source+kind).
- Multi-currency quote conversion, per-device differing lists, on-device ticker editing.

---

## Section 1 — Architecture Overview

```
 macOS Hub (menubar)                         Device (ESP32-S3)
 -------------------                         -----------------
 Source adapters (Binance/Yahoo)
   -> unified search panel
   -> user curates desired list (<=16)
   -> resolve to canonical tuples
   -> persist in hub prefs
        |  BLE: {"v":1,"config":{rev,tickers[]}}
        v
                                            hub_proto: parse config frame
                                              -> validate (enums, count)
                                              -> write NVS blob
                                              -> swap runtime ticker table (locked)
                                              -> reseed DataStore finance slots
                                              -> reset fetch scheduler
                                              -> bump finance_config_gen
                                            Finance screen rebuilds view (Core1)
        ^  BLE: {"v":1,"ack":"config",rev,ok,...}
        |
   menubar sync status ("Synced" / error)

 (unchanged) device fetches live prices over WiFi per runtime table
```

The device never knows the hub catalog exists; it only ever receives resolved tuples and applies them.

---

## Section 2 — Contract (additive; frozen blocks untouched)

Append a new section to `hub/CONTRACT.md`. Keep `firmware/src/core/hub_proto.cpp`, `hub/Sources/BeaconHubKit/Protocol.swift`, and their tests in sync.

### Hub => Device: config frame (chunked full snapshot)

A full 16-row snapshot exceeds the BLE frame limits (see frame-size note), so the snapshot is **chunked**. Each part is a self-contained frame; the device accumulates parts for a `rev` and applies on the last part.

```json
{"v":1,"config":{"rev":7,"part":0,"parts":2,"tickers":[
  {"id":"bz_btcusdt","src":"binance","sym":"BTCUSDT","name":"BTC","kind":"crypto","cadence":60,"stale":600,"basis":"24h"}
]}}
{"v":1,"config":{"rev":7,"part":1,"parts":2,"tickers":[
  {"id":"yh_gspc","src":"yahoo","sym":"%5EGSPC","name":"S&P 500","kind":"index","cadence":300,"stale":600,"basis":"prev_close"}
]}}
```

- `rev` — hub's monotonic revision (uint). Echoed in the ack; correlates ack to push.
- `part` / `parts` — 0-based chunk index and total. Hub packs as many whole rows per frame as fit under the chunk budget; never splits a row across frames.
- `tickers` — ordered; concatenated across parts in `part` order == display order. Full replace.
- **Device accumulation:** buffer rows for the in-progress `rev`; on `part == parts-1` validate + apply the assembled list. A new `rev` arriving mid-sequence discards the partial. A missing/duplicate part (gap) discards the partial and the device acks `bad_chunking` so the hub re-sends. The whole snapshot must arrive within a short window (e.g. 5 s) or the partial is dropped.

### Device => Hub: config ack (uses the device=>hub `cmd` channel)

The device=>hub direction uses `cmd` (cf. `{"cmd":"permission"}`); `ack`/`err` are reserved for the hub=>device direction. The config ack therefore uses a distinct `cmd` discriminator — it does **not** overload the existing prompt-id `ack`. One ack per completed snapshot (`rev`).

```json
{"v":1,"cmd":"config_ack","rev":7,"ok":true,"count":8}
{"v":1,"cmd":"config_ack","rev":7,"ok":false,"err":"too_many_tickers"}
```

- On reject the device keeps its current list (fail closed). `err` in:
  `too_many_tickers` (> MAX_TICKERS=16), `empty` (0 tickers), `bad_source`, `bad_kind`, `bad_basis`, `bad_chunking`, `nvs_write_failed`, `malformed`.
- **Hub side:** inbound device frames currently parse only `DeviceCommand` (`BeaconCentral.swift:197`). Add a `config_ack` variant + tests.
- **Device side:** `onFrame` (`hub_task.cpp:44`) dispatches `ack`/`err` => loc => status today; add a `config` branch **before** the loc/status fall-through (a config frame has neither `ack` nor `err`, so without it the frame is silently ignored).

### Enum string <=> firmware enum mapping (pin in CONTRACT.md)

| Frame `src` | firmware `ticker_source_t` |
|---|---|
| `binance` | `SRC_BINANCE` |
| `yahoo` | `SRC_YAHOO` |

(`frankfurter` / `SRC_FRANKFURTER` removed — see Section 6.)

| Frame `kind` | firmware `ticker_kind_t` |
|---|---|
| `fx` | `KIND_FX` (renamed from `KIND_FX_IDR` as part of Frankfurter cleanup — the IDR suffix was tied to Frankfurter's hardcoded IDR quote; FX now means Yahoo `=X` pairs. Amends FR-STATE-0; see Section 6.) |
| `crypto` | `KIND_CRYPTO` |
| `index` | `KIND_INDEX` |
| `etf` | `KIND_ETF` |

| Frame `basis` | firmware `change_basis_t` |
|---|---|
| `prev_close` | `CHG_PREV_CLOSE` |
| `24h` | `CHG_24H` |

### Frame-size note (drives the chunking decision)

Two hard limits in firmware:
- `HUB_FRAME_MAX = 1024` — the reassembler's `buf[]` (`hub_proto.h:17`); longer frames are dropped.
- `HUB_SB_BYTES = 2048` — the inbound stream buffer (`hublink_ble.cpp:27`).

A worst-case 16-row snapshot is ~2.0–2.7 KB (full key names; `sym`/`name` up to their length bounds), which exceeds **both** limits — so a single frame is impossible without growing `buf[]` past the stream buffer. Hence **chunking is mandatory** (not optional). Keep each chunk <= ~900 B (margin under `HUB_FRAME_MAX`); ~4–6 rows per frame, <= 4 frames for a full 16-row list. No firmware buffer needs to grow.

---

## Section 3 — Firmware

### 3.1 Runtime ticker table

Replace direct `DEFAULT_TICKERS[i]` reads with a runtime, owned-storage table.

```c
// firmware/src/config/tickers.h (new runtime type; DEFAULT_TICKERS kept as fallback seed)
typedef struct {
  char            id[FIN_ID_LEN];       // 16
  ticker_source_t source;
  char            symbol[TKR_SYM_LEN];  // source-specific, URL-ready (e.g. "%5EGSPC")
  char            name[TKR_NAME_LEN];   // display name
  ticker_kind_t   kind;
  uint16_t        cadence_s;
  uint32_t        stale_s;
  change_basis_t  change_basis;
} ticker_runtime_t;

// All accessors are guarded by a dedicated ticker-table rwlock owned by this module
// (NOT the private DataStore s_lock). Readers copy the row they need, then release.
int  tickers_count(void);                                  // active count
bool tickers_get(int i, ticker_runtime_t* out);            // copies row i under lock; false if i out of range
uint32_t tickers_config_gen(void);                         // bumps on every successful swap
```

- The runtime table lives behind this module with its own mutex (Codex #5: `s_lock` is private to `datastore.cpp` and does not cover ticker config). Consumers **copy** the row under the lock rather than holding a pointer across a blocking call.
- Choose `TKR_SYM_LEN` / `TKR_NAME_LEN` to bound the worst case (e.g. 24 / 24); enforce on parse — **reject** with `malformed` (no silent truncation).
- All consumers switch to accessors: `firmware/src/fetch/finance.cpp` (URL build, cadence, basis), `firmware/src/core/fetch_task.cpp` (slot count, cadence, host), and every per-theme finance view in `firmware/src/ui/screens/views/` (display name — they currently read `DEFAULT_TICKERS` directly, e.g. `finance_calm.cpp:16`, `finance_editorial.cpp:8`).

### 3.2 NVS persistence

- Versioned **packed binary blob** under a dedicated NVS key: `schema_ver` + `count` + `crc32` + entries.
- **Persist source/kind/basis as stable wire codes** (the string enums, or a fixed code table that never reorders), **not** raw C enum ordinals (Codex #8). The Frankfurter removal shifts `ticker_source_t` ordinals; a blob keyed on ordinals would silently misread after the shift. `schema_ver` gates migration if the layout ever changes.
- Boot order: read NVS => validate (`schema_ver` match, `crc32` match, `count` in `[1,MAX_TICKERS]`, every code maps to a known enum, lengths within bounds) => if all valid use it, else seed from `DEFAULT_TICKERS`.
- On valid push: write NVS first; **check the write return code** (existing `nvs.cpp` ignores it). On write failure, do not swap the RAM table; ack `nvs_write_failed`. Only after a confirmed write do we swap in RAM.

### 3.3 Live re-apply (Core0 receives, Core1 renders)

On a validated, fully-assembled snapshot (after a confirmed NVS write):

1. Build the new table into a staging buffer (parse already validated each row).
2. Under the **ticker-table lock**: swap the active table, bump `tickers_config_gen`.
3. Reseed the DataStore finance slots under the DataStore lock: new `id`s, `ST_LOADING`, count = new count.
4. Signal the fetch scheduler to rebuild (see below).

**Stale-fetch race (Codex #1 — Critical).** Today `ds_set_finance(idx, ...)` is index-only and writes whatever slot `idx` currently holds (`datastore.cpp:40`). An in-flight HTTPS fetch started before the swap could land its old value into the freshly reseeded slot and mark it `ST_LIVE`. Fix: each fetch captures `tickers_config_gen` (and the row `id`) at start; `ds_set_finance` takes the expected `id`/gen and **drops the publish** if it no longer matches. Stale results are discarded, not shown.

**Dynamic scheduler (Codex #2 — Critical).** `fetch_task` fixes `slots = 1 + DEFAULT_TICKERS_COUNT` once at boot and reads cadence/host from `DEFAULT_TICKERS` (`fetch_task.cpp:53,25,34`). It must instead derive the finance slot set, cadences, and host from the runtime table, and rebuild that set when `tickers_config_gen` changes (add/remove slots, reset timers). The weather/other non-finance slots are unaffected.

**Finance UI rebuild (Codex #6).** The only existing rebuild hook is theme-switch (`carousel.cpp`); finance views build row objects once in `build()` and cache `s_rows` (`finance_calm.cpp:59`), reading names from `DEFAULT_TICKERS`. Add a Core1-safe path: the Finance screen polls `tickers_config_gen` on its update tick and, on change, runs destroy => build => update against the new count/names (names via the accessor). This reuses the existing per-screen build/update/destroy lifecycle.

All cross-core handoff is via copied snapshots under the relevant lock — never block the LVGL loop, never hold a lock across a fetch or a render.

### 3.4 Validation (device-side, fail closed)

- chunking: contiguous `part` 0..`parts`-1 for one `rev`, arriving within the window, else `bad_chunking`.
- count in `[1, MAX_TICKERS]` else `empty` / `too_many_tickers`.
- every `src`/`kind`/`basis` maps to a known code else `bad_*`.
- `id`/`symbol`/`name` within length bounds, `id` non-empty else `malformed`.
- NVS write must succeed else `nvs_write_failed`.
- On any failure: do not touch NVS or the active table; ack `ok:false` with the first error. The current list keeps running.

---

## Section 4 — Hub

### 4.1 Source adapters (BeaconHubKit, host-testable with fixtures)

Each adapter maps a source's universe to candidate canonical tuples.

| Source | Discovery | Caching | Mapping defaults |
|---|---|---|---|
| **Binance** | `GET /exchangeInfo` | cache to disk, ~daily refresh; filter `status=TRADING`, quote in {USDT,USDC,FDUSD} | `kind=crypto`, `basis=24h`, `cadence=60`, `sym` raw (`BTCUSDT`) |
| **Yahoo** | **live search** endpoint (debounced) | none (live) | `quoteType` => kind (INDEX=>index, ETF=>etf, CRYPTOCURRENCY=>crypto, CURRENCY=>fx, EQUITY=>etf-or-equity); `basis=prev_close`, `cadence=300`; `sym` **URL-encoded** (`^GSPC` => `%5EGSPC`, `EUR/USD` => `EURUSD=X`) |

FX is covered entirely by Yahoo `=X` symbols (live, any pair incl. `USDIDR=X`). A **defaults table keyed by (source, kind)** supplies `cadence_s` / `stale_s` / `basis` so the user never sets them.

### 4.2 Unified search (decision #6)

- One search box. On query: Yahoo queried live (debounced); Binance filtered from cache. Results merged, grouped/labeled by source.
- **Crypto quote (decision #8):** typing a base surfaces `<base>/USDT` first, plus USDC/FDUSD rows.
- **FX:** handled by Yahoo `=X`. A query that parses as a currency pair (contains `/` or two known ISO codes, e.g. `USD/IDR`) maps to the Yahoo `USDIDR=X` row; plain free text => Yahoo/Binance as matched.
- **Duplicates (decision #7):** the same logical ticker from two sources shows as two labeled rows; user picks.

### 4.3 Correctness risks (must be test-covered)

1. **Symbol encoding — exact-once (Codex #10).** Firmware interpolates the Yahoo `symbol` straight into the URL path with no escaping (`finance.cpp:63`). The hub must emit the path-segment-encoded form **exactly once** (`^GSPC` => `%5EGSPC`, `EUR/USD` => `EURUSD=X`): a raw `/` breaks the path; double-encoding breaks Yahoo. Pin with contract fixtures asserting the exact bytes the firmware expects.
2. **Stable `id` — deterministic, not list-local (Codex #9).** `FIN_ID_LEN`=16 (incl. NUL), and `tickers.h` requires ids be "stable; never reuse." Derive the `id` deterministically from `(src, canonical symbol)` — e.g. a short hash — so it is **invariant under reorder/removal**. Do **not** use a list-position collision suffix (that would change ids when the list changes and break DataStore/UI linkage + the `ST_LOADING` continuity).

### 4.4 Desired list, encode, push, ack

- Desired list = ordered resolved tuples, persisted in hub prefs (no static catalog to reference).
- Menubar editor: add from search, remove, reorder; enforce <= 16.
- Encoder (`Protocol.swift`): build config frame, bump `rev`.
- **Push** on BLE connect (re-sync rebooted/re-paired device) and on every edit.
- **Ack** updates menubar sync status: "Synced" on `ok:true`; on `ok:false` show the `err` and keep the prior synced state flagged stale.

---

## Section 5 — Testing

### Firmware (native / Unity)
- Config frame parse: valid; each invalid enum (`bad_source`/`bad_kind`/`bad_basis`); `too_many_tickers`; `empty`; `malformed` (bad lengths, missing id). Table-driven.
- **Chunk reassembly:** in-order parts assemble; new `rev` mid-sequence discards partial; gap/duplicate part => `bad_chunking`; window timeout drops partial.
- NVS blob round-trip: write => read => equal; `schema_ver` mismatch => fallback; bad `crc32` => fallback; stable-code decode survives a simulated enum-ordinal shift.
- Table swap/validation: applied atomically under lock; reject path leaves active table + NVS untouched; `tickers_config_gen` bumps only on success.
- **Stale-fetch guard:** a publish carrying a stale `id`/gen is dropped, not written (regression for Codex #1).

### Hub (BeaconHubKit)
- Adapter mapping from fixtures (Binance exchangeInfo, Yahoo search) => canonical tuples with correct defaults.
- Unified merge: dedup labeling, crypto quote ordering (USDT first), FX pair parsing.
- **Symbol encoding exact-once** (Yahoo `%5E...`, no double-encode, no raw `/`) against firmware fixtures.
- **Stable `id` invariance:** same `(src, symbol)` => same id across reorder/removal.
- Codec round-trip: encode chunked config frames in Swift; assert each chunk <= budget and the JSON shape matches the firmware parser; **`config_ack` parse** (ok/err/rev) on the hub inbound path.
- `<= 16` enforcement; rev monotonicity; chunk packing (no row split, parts count correct).

### Contract sync
- A shared round-trip test asserting firmware and Swift agree on the frame schema per `CONTRACT.md` (extend the existing protocol-sync test).

---

## Section 6 — Frankfurter Removal (cleanup)

Frankfurter is hardcoded to IDR quotes and serves lagging once-daily ECB rates; Yahoo `=X` covers live FX for any pair (incl. `USDIDR=X`). Remove it from the firmware to keep the trace clean. This amends the FR-STATE-0 schema, so authoritative docs must follow.

**Firmware deletions:**
- `firmware/src/config/tickers.h` — remove `SRC_FRANKFURTER` from `ticker_source_t`; rename `KIND_FX_IDR` => `KIND_FX`; drop the IDR-suffix comment.
- `firmware/src/fetch/finance.cpp` — remove `fetch_frankfurter()` and its dispatch `case SRC_FRANKFURTER`.
- `firmware/src/fetch/parse_finance.h` / `parse_finance.cpp` — remove `parse_frankfurter()` and `parse_frankfurter_series()`.
- `firmware/src/core/fetch_task.cpp` — remove the `case SRC_FRANKFURTER: return "api.frankfurter.dev"` host mapping.

**Test updates:**
- `firmware/test/test_finance_parse/test_main.cpp` — remove the 4 Frankfurter tests + their `RUN_TEST`s.
- `firmware/test/test_config/test_main.cpp` — update the kind range check to `KIND_FX`.

**Comment / doc references the first sweep missed (Codex #8):**
- `firmware/src/config/root_ca.h:5-6` — comment credits GlobalSign/Let's Encrypt to "Frankfurter". **Keep the CA root** (Open-Meteo shares Let's Encrypt — verified `root_ca.h:6`); update the comment to drop Frankfurter, no root removed.
- `docs/tech.md:113` (config schema `source` enum — drop `frankfurter`) and `docs/tech.md:127` (FX row — drop the "Frankfurter kept as daily-ECB option" note).
- `hub/CONTRACT.md` — when adding the config frame, reflect the 2-source `src` enum.

Leave dated plan/spec records (`docs/plans/*`, `docs/specs/2026-06-06*`, `2026-06-08*`) as-is — historical.

**Notes:**
- Removing the first enumerator shifts `SRC_BINANCE`/`SRC_YAHOO` ordinals; both switch statements dispatch by name, so live code is safe. **The NVS blob must persist stable codes, not ordinals** (see 3.2) so a future enum reorder cannot silently misread persisted config.
- `KIND_FX_IDR` is not referenced by UI views (verified) — rename touches only `tickers.h` + `test_config` (range check at `test_config/test_main.cpp:30-31`).

## Remaining Confirmation

- **`TKR_SYM_LEN` / `TKR_NAME_LEN`** — finalize bounds against real worst-case symbols/names (Yahoo `=X` / encoded `^` symbols, longest display names). Verify during planning.
