# Hub-Driven Ticker Configuration — Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the device's ticker list runtime-configurable from the hub over BLE — parse a chunked `config` snapshot, persist it to NVS, and live-apply it to fetchers + the Finance screen without a reboot.

**Architecture:** Replace compile-time `DEFAULT_TICKERS[]` reads with a runtime ticker table behind a dedicated lock (defaults are the boot fallback). A new additive BLE `config` frame (chunked) is validated, persisted as a versioned packed NVS blob (stable codes, crc32), then hot-swapped; the Core-0 fetch scheduler rebuilds from the table on a generation bump, and the Core-1 Finance screen rebuilds its view. Frankfurter (lagging daily-ECB, IDR-only) is removed first as a self-contained cleanup.

**Tech Stack:** C++ (Arduino-ESP32 core 3.3.5, pioarduino), LVGL 8.4, ArduinoJson, NVS (Preferences), Unity (native host tests). BLE via core `BLE*` wrapper (NimBLE-backed).

**Scope:** Firmware only. The hub (source adapters, search UI, encoder/push, `config_ack` parsing) is a **separate follow-up plan** that consumes the contract frozen here. Spec: `docs/specs/2026-06-17-hub-ticker-config-design.md`. Issue #92.

## Global Constraints

- Do NOT bump the pinned toolchain (Arduino-ESP32 core 3.3.5, GFX 1.6.4, LVGL 8.4.0). Use only the core `BLE*` wrapper — never add NimBLE-Arduino.
- Native host tests run in the `native` env (Unity, pure C++); device-only code is guarded with `#if !BEACON_NATIVE`.
- PlatformIO runs only from `~/.beacon-pio/bin/pio`.
- `MAX_TICKERS = 16`; `FIN_ID_LEN = 16` (id ≤ 15 chars + NUL). One TLS socket at a time; Core-0 = fetch/BLE, Core-1 = LVGL render only; cross-core only via locked snapshots — never block the UI loop.
- Conventional Commits: `type(scope): subject` (scope `firmware`). Commit per task; the USER commits — the agent stages and proposes, the user runs the final commit. (For this plan's TDD cadence the agent MAY commit locally on the feature branch; do not push.)
- BLE frame limits: `HUB_FRAME_MAX = 1024` (reassembler), `HUB_SB_BYTES = 2048` (stream buffer). Config snapshot MUST be chunked; each chunk ≤ ~900 B.
- Persist source/kind/basis as **stable codes**, never raw C enum ordinals (the Frankfurter removal shifts ordinals).

---

## Chunk Roadmap (dependency order)

| Chunk | Deliverable | Mergeable alone? |
|---|---|---|
| **A1** | Frankfurter removed + `KIND_FX_IDR` => `KIND_FX`; tests + docs updated | Yes |
| **A2** | Runtime ticker table + dedicated lock + accessors; all consumers read the table (seeded from defaults) | Yes (no behavior change) |
| **A3** | NVS persistence (packed blob, stable codes, crc32, schema_ver); load-or-default at boot | Yes |
| **A4** | BLE `config` frame parse (chunked) + validation + `config_ack` build; `onFrame` dispatch branch | Yes (parse/ack only, not yet applied) |
| **A5** | Live re-apply: locked swap, DataStore reseed, dynamic scheduler rebuild, stale-fetch guard, Finance UI rebuild | Yes (completes firmware) |

Each chunk ends green (`~/.beacon-pio/bin/pio test -e native` passes) and, where it touches device code, compiles (`~/.beacon-pio/bin/pio run`).

---

## Chunk A1: Remove Frankfurter + rename KIND_FX_IDR => KIND_FX

**Rationale:** Self-contained, low-risk, unblocks the enum rename everything else builds on. Frankfurter is hardcoded to IDR quotes (`?base=<sym>&symbols=IDR`) and serves lagging daily-ECB rates; live FX is served by Yahoo `=X`.

**Files:**
- Modify: `firmware/src/config/tickers.h` (enum `ticker_source_t`, `ticker_kind_t`, comments)
- Modify: `firmware/src/fetch/finance.cpp:26-44,79` (remove `fetch_frankfurter` + dispatch case)
- Modify: `firmware/src/fetch/parse_finance.h:14-20` (remove decls)
- Modify: `firmware/src/fetch/parse_finance.cpp:16-42` (remove `parse_frankfurter`, `parse_frankfurter_series`)
- Modify: `firmware/src/core/fetch_task.cpp:34-39` (remove `SRC_FRANKFURTER` host case)
- Modify: `firmware/test/test_finance_parse/test_main.cpp` (remove 4 Frankfurter tests + RUN_TESTs)
- Modify: `firmware/test/test_config/test_main.cpp:30-31` (range check uses `KIND_FX`)
- Modify: `firmware/src/config/root_ca.h:5-6` (comment only — keep all certs)
- Modify: `docs/tech.md:113,127` (drop `frankfurter`; FX row note)

**Interfaces:**
- Produces: `ticker_source_t { SRC_BINANCE, SRC_YAHOO }`; `ticker_kind_t { KIND_FX, KIND_CRYPTO, KIND_INDEX, KIND_ETF }`. Later chunks map wire `src` ∈ {binance,yahoo} and wire `kind` ∈ {fx,crypto,index,etf} to these.

- [ ] **Step 1: Update the parse tests first (remove Frankfurter cases)**

In `firmware/test/test_finance_parse/test_main.cpp`, delete the four functions `test_frankfurter_rate`, `test_frankfurter_missing_idr`, `test_frankfurter_series`, `test_frankfurter_series_single_day`, and their `RUN_TEST(...)` lines (keep all binance/yahoo tests). The `main()` should run only binance + yahoo tests.

- [ ] **Step 2: Run the suite to confirm it still builds/passes without the Frankfurter tests** (parsers still exist, so this is green)

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_finance_parse*"`
Expected: PASS (binance + yahoo only).

- [ ] **Step 3: Remove the Frankfurter parsers**

In `firmware/src/fetch/parse_finance.h` delete the `parse_frankfurter` (line ~15) and `parse_frankfurter_series` (line ~20) declarations and their comment blocks. In `firmware/src/fetch/parse_finance.cpp` delete both function bodies (lines ~16-42). Keep `parse_binance` and `parse_yahoo`.

- [ ] **Step 4: Remove the Frankfurter fetcher + dispatch**

In `firmware/src/fetch/finance.cpp` delete `fetch_frankfurter` (lines ~26-44, incl. the `<time.h>`-only usage if now unused — verify `time.h` still needed by other code in the file; it is not after removal, so drop the `#include <time.h>` at line 12). In `fetch_finance` (line ~75) remove `case SRC_FRANKFURTER: return fetch_frankfurter(idx, c);`.

- [ ] **Step 5: Remove the scheduler host case**

In `firmware/src/core/fetch_task.cpp` `slot_host()` remove `case SRC_FRANKFURTER: return "api.frankfurter.dev";`. Leave `default: return "";`.

- [ ] **Step 6: Rename the enums + fix tickers.h comments**

In `firmware/src/config/tickers.h`:
- `typedef enum { SRC_BINANCE, SRC_YAHOO } ticker_source_t;` (drop `SRC_FRANKFURTER`).
- `typedef enum { KIND_FX, KIND_CRYPTO, KIND_INDEX, KIND_ETF } ticker_kind_t;`
- Replace `KIND_FX_IDR` with `KIND_FX` in all `DEFAULT_TICKERS` rows (the four FX rows at lines ~28-31).
- Update the comment block at lines ~25-27 to drop the Frankfurter/IDR explanation (FX is Yahoo `=X`, near-live).

- [ ] **Step 7: Update the config test range check**

In `firmware/test/test_config/test_main.cpp:30-31`:
```c
TEST_ASSERT_TRUE(t->source >= SRC_BINANCE && t->source <= SRC_YAHOO);
TEST_ASSERT_TRUE(t->kind >= KIND_FX && t->kind <= KIND_ETF);
```

- [ ] **Step 8: Fix root_ca.h comment (keep all certs)**

In `firmware/src/config/root_ca.h:5-6` reword to drop Frankfurter, e.g.:
```c
//        GlobalSign Root CA, Starfield Services Root CA G2 (Binance data mirror).
// Covers Open-Meteo / ipwho.is / BigDataCloud (Let's Encrypt) + Binance / Yahoo (DigiCert) + rotation headroom.
```
Do NOT remove any certificate block — roots are shared across hosts.

- [ ] **Step 9: Update tech.md**

`docs/tech.md:113`: `source: "binance"|"yahoo"` and `kind: "fx"|"crypto"|"index"|"etf"`.
`docs/tech.md:127` (the FX row): drop "Frankfurter kept as a daily-ECB option" — leave `FX → IDR (Yahoo <X>IDR=X — near-live)`.

- [ ] **Step 10: Run the full native suite**

Run: `~/.beacon-pio/bin/pio test -e native`
Expected: PASS (all suites; no Frankfurter symbols referenced anywhere).

- [ ] **Step 11: Compile for device (no link regressions)**

Run: `~/.beacon-pio/bin/pio run`
Expected: build succeeds.

- [ ] **Step 12: Commit**

```bash
git add firmware/src firmware/test docs/tech.md
git commit -m "refactor(firmware): drop Frankfurter source, rename KIND_FX_IDR => KIND_FX"
```

---

## Chunk A2: Runtime ticker table + lock + accessors

**Deliverable:** A runtime, owned-storage table seeded from `DEFAULT_TICKERS`, behind a dedicated lock, with copy-out accessors and a generation counter. All consumers (finance fetch, scheduler, finance views) read the table instead of `DEFAULT_TICKERS`. No behavior change yet.

**Files:**
- Create: `firmware/src/config/ticker_table.h` / `firmware/src/config/ticker_table.cpp`
- Create: `firmware/test/test_ticker_table/test_main.cpp`
- Modify: `firmware/src/fetch/finance.cpp` (use accessor; copy row before fetch), `firmware/src/core/fetch_task.cpp` (`cadence_of`/`slot_host`/`slots` via accessors + runtime count), finance views in `firmware/src/ui/screens/views/finance_*.cpp` (display name via accessor)

**Interfaces:**
- Produces:
  ```c
  typedef struct { char id[FIN_ID_LEN]; ticker_source_t source; char symbol[TKR_SYM_LEN];
                   char name[TKR_NAME_LEN]; ticker_kind_t kind; uint16_t cadence_s;
                   uint32_t stale_s; change_basis_t change_basis; } ticker_runtime_t;
  void     ticker_table_init(void);                       // seed from DEFAULT_TICKERS
  int      ticker_table_count(void);
  bool     ticker_table_get(int i, ticker_runtime_t* out);// copy under lock; false if OOB
  uint32_t ticker_table_gen(void);                        // bumps on swap (A5)
  ```
- `TKR_SYM_LEN`/`TKR_NAME_LEN`: set from worst case (Yahoo encoded symbols like `%5EGSPC`, longest display name `OIL WTI`); use `TKR_SYM_LEN = 24`, `TKR_NAME_LEN = 24` — **confirm** against the default set during this chunk and document.

**Tasks (TDD, authored in full at execution time):** test seeds count == `DEFAULT_TICKERS_COUNT` and row 0 fields match `DEFAULT_TICKERS[0]`; `ticker_table_get` OOB returns false; accessor returns a copy (mutating it doesn't affect the table); `ticker_table_gen` starts at 0. Then implement; then switch each consumer; then `pio test -e native` + `pio run`; commit `feat(firmware): runtime ticker table behind a lock`.

---

## Chunk A3: NVS persistence

**Deliverable:** Versioned packed blob (`schema_ver`, `count`, `crc32`, entries with **stable codes** for source/kind/basis). Load-or-default at boot; reject corrupt/old blobs to defaults.

**Files:**
- Create: `firmware/src/config/ticker_store.h` / `.cpp` (serialize/deserialize + crc32 + code<->enum mapping)
- Create: `firmware/test/test_ticker_store/test_main.cpp`
- Modify: `firmware/src/config/ticker_table.cpp` (boot: `ticker_store_load()` else seed defaults), persistence via existing `core/nvs`

**Interfaces:**
- Produces:
  ```c
  // stable wire codes, independent of C enum ordinals
  // src: 1=binance 2=yahoo; kind: 1=fx 2=crypto 3=index 4=etf; basis: 1=prev_close 2=24h
  bool ticker_store_save(const ticker_runtime_t* rows, int count);   // returns false on NVS write fail
  int  ticker_store_load(ticker_runtime_t* out, int max);            // -1 if absent/invalid; else count
  ```
- crc32 over the packed body; `schema_ver = 1`.

**Tasks:** round-trip equal; bad crc => -1; wrong schema_ver => -1; a row encoded with stable codes decodes correctly even when C enum ordinals are simulated-shifted (compile-time guard test); save returns false path stubbed via an injectable write. Then `pio test -e native`; commit `feat(firmware): NVS persistence for ticker config`.

---

## Chunk A4: BLE config frame parse + ack

**Deliverable:** Parse the chunked `config` frame into a staged row array (host-testable), validate per §3.4, build the `config_ack` command, and dispatch a `config` branch in `onFrame`. Not yet applied to the live table.

**Files:**
- Modify: `firmware/src/core/hub_proto.h` / `hub_proto.cpp` (add `hub_parse_config_chunk`, `hub_build_config_ack`, a `config_accumulator_t`)
- Modify: `firmware/src/core/hub_task.cpp:44` (dispatch `config` before loc/status)
- Create: `firmware/test/test_hub_config/test_main.cpp`

**Interfaces:**
- Produces:
  ```c
  typedef struct { uint32_t rev; int part, parts; ticker_runtime_t rows[MAX_TICKERS]; int row_count; } config_chunk_t;
  // returns ERR_NONE + fills chunk; ERR_PARSE on malformed JSON
  data_err_t hub_parse_config_chunk(const char* json, size_t len, config_chunk_t* out);
  // accumulate across parts; returns: PENDING, DONE(rows,count), or an err code (bad_chunking/too_many/empty/bad_*)
  // emits the config_ack frame via hub_build_config_ack(buf, cap, rev, ok, err, count)
  ```
- Validation errors map to ack `err` strings: `too_many_tickers,empty,bad_source,bad_kind,bad_basis,bad_chunking,malformed` (NVS/apply errors come in A5).

**Tasks:** table-driven parse/validate tests (valid single-part; multi-part assembly; new-rev-resets-partial; gap => bad_chunking; each bad enum; too many; empty; over-length field => malformed); ack build (ok + each err). Then dispatch wiring (device-only, `#if !BEACON_NATIVE` for the BLE send). `pio test -e native` + `pio run`; commit `feat(firmware): parse chunked hub config frame + config_ack`.

---

## Chunk A5: Live re-apply

**Deliverable:** On a validated full snapshot: write NVS (ack `nvs_write_failed` on failure), swap the table under lock + bump gen, reseed DataStore finance slots, rebuild the Core-0 scheduler from the table, guard publishes against stale `(id,gen)`, and rebuild the Core-1 Finance view on gen change.

**Files:**
- Modify: `firmware/src/config/ticker_table.cpp` (`ticker_table_apply(rows,count)` => save+swap+gen)
- Modify: `firmware/src/core/datastore.h/.cpp` (`ds_reseed_finance(ids,count)`; `ds_set_finance` gains expected-id/gen guard — see below)
- Modify: `firmware/src/fetch/finance.cpp` (capture gen+id at fetch start; pass to guarded publish)
- Modify: `firmware/src/core/fetch_task.cpp` (rebuild `slots`/`s_next_due` when `ticker_table_gen()` changes)
- Modify: `firmware/src/ui/screens/screen_finance.cpp` (+ views) (poll `ticker_table_gen()`; destroy=>build=>update on change)
- Modify: `firmware/src/core/hub_task.cpp` (A4 DONE => `ticker_table_apply` => ack ok/`nvs_write_failed`)
- Tests: extend `test_datastore` (reseed + stale-publish guard), `test_ticker_table` (apply bumps gen, reject leaves table intact)

**Interfaces:**
- Consumes: A2 `ticker_table_*`, A3 `ticker_store_save`, A4 `config_chunk_t`/DONE.
- Produces:
  ```c
  bool ticker_table_apply(const ticker_runtime_t* rows, int count); // save+swap+gen; false on NVS fail
  void ds_reseed_finance(const char ids[][FIN_ID_LEN], int count);
  // guarded publish: drop if the slot's id changed since fetch start
  void ds_set_finance_if(uint8_t idx, const char* expect_id, const finance_rec_t* f);
  ```

**Tasks (host-testable portions):** `ds_set_finance_if` drops a publish when `expect_id` mismatches (Codex #1 regression); `ds_reseed_finance` sets new ids + `ST_LOADING`; `ticker_table_apply` bumps gen and persists, reject path leaves table+NVS intact. Device-only integration (scheduler rebuild, Finance UI rebuild) verified on-device per the manual checklist below. `pio test -e native` + `pio run`; commit `feat(firmware): live-apply hub ticker config (scheduler + UI rebuild)`.

**On-device manual verification (A5):**
- Push a 3-ticker list => Finance screen shows exactly those 3, fetchers start, slots go `ST_LOADING` => `ST_LIVE`.
- Reboot with hub off => same 3 persist (NVS).
- Push an invalid list (e.g. 17 rows) => device keeps the current list, hub sees `too_many_tickers`.
- Heap stays ≥ ~53 KB min free internal during a push under BLE+WiFi.

---

## Self-Review

- **Spec coverage:** Contract (§2) => A4 (+ hub follow-up for encoder/push); Firmware runtime table (§3.1) => A2; NVS (§3.2) => A3; live re-apply + stale-fetch + scheduler + UI (§3.3) => A5; validation (§3.4) => A4/A5; Frankfurter removal (§6) => A1. Hub (§4) + the ack's hub-side parsing => follow-up hub plan. Testing (§5) firmware rows => per-chunk tests.
- **Placeholder scan:** A1 is fully stepped. A2–A5 carry exact files + interface signatures + test intent; their per-step TDD code is authored against live code immediately before each chunk executes (avoids speculative/inaccurate code), consistent with subagent-driven execution.
- **Type consistency:** `ticker_runtime_t`, `ticker_table_gen`/`ticker_table_get`, `ds_set_finance_if`, `config_chunk_t` used consistently across A2/A4/A5. Stable codes defined once in A3 and reused by A4/A5.
