# Device -> Hub Ticker Report Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a fresh (never-configured) hub adopt the device's existing ticker list on pairing, by adding an additive device -> hub `cmd:"report"` frame.

**Architecture:** The device serializes its running `ticker_table` into chunked `cmd:"report"` frames (mirroring the frozen `config` row schema) and emits them once per connection after the first inbound hub frame. A pristine hub (`rev == 0 && rows.isEmpty`) reassembles them and adopts the list (persist `rev 0 -> 1`, refresh panel, mark `synced`, no push-back); any non-pristine hub ignores the report and stays the source of truth.

**Tech Stack:** Firmware C++ (ArduinoJson v7, Unity tests in the `native` PlatformIO env); Hub Swift 6 (SwiftPM, XCTest in `BeaconHubKit`).

**Design doc:** `docs/specs/2026-06-19-device-ticker-report-design.md`

## Global Constraints

- Additive only: the frozen `config` / `config_ack` / status / buddy / loc / permission frames are untouched (`hub/CONTRACT.md` §A/§B/§B2; `docs/tech.md` §7.1).
- New frame rides the **device -> hub `cmd` channel**: `{"v":1,"cmd":"report","what":"tickers","rev":0,"part":P,"parts":N,"tickers":[...]}`.
- `rev` is **always `0`** from the device (it does not persist the hub's rev).
- Row schema/caps identical to `config`: `id` <=15, `sym` <=23, `name` <=23 UTF-8 bytes; `src` (`binance`|`yahoo`), `kind` (`fx`|`crypto`|`index`|`etf`), `basis` (`prev_close`|`24h`); `cadence`/`stale` ints.
- Chunking: each newline-terminated line <= ~900 B (margin under firmware `HUB_FRAME_MAX` = 1024); a row is never split; <= `MAX_TICKERS` (16) rows total. Encoder appends a trailing `0x0A`.
- All-or-zero: a report is emitted in full or not at all; never partial.
- The report is one-way (no ack). Adoption never calls `pushTickerConfig()` (avoid an echo loop).
- Firmware: PlatformIO from the venv `~/.beacon-pio/bin/pio`; host tests run in the `native` env (no Arduino/BLE; device-only code guarded `#if !BEACON_NATIVE`).
- Conventional Commits, scopes `firmware` / `hub` / `docs` (`CONTRIBUTING.md`). Do not commit on the user's behalf beyond the per-task commits this plan calls for on a feature branch.

---

### Task 1: Firmware report serializer (`hub_report_plan` + `hub_build_report_frame`)

Pure, host-testable codec in `hub_proto.cpp`: enum -> wire helpers, a single-frame serializer, and a greedy chunk planner. No device/BLE deps.

**Files:**
- Modify: `firmware/src/core/hub_proto.h` (declare the two functions, after `hub_build_config_ack` at `:119`)
- Modify: `firmware/src/core/hub_proto.cpp` (implement, after `hub_build_config_ack` at `:280`)
- Create: `firmware/test/test_hub_report/test_main.cpp`

**Interfaces:**
- Consumes: `ticker_runtime_t` (`config/ticker_table.h`), `MAX_TICKERS` (`config/tickers.h`), `HUB_FRAME_MAX` (`hub_proto.h`), `finish_frame` (static in `hub_proto.cpp`).
- Produces:
  - `int hub_report_plan(const ticker_runtime_t* rows, int count, int group_start[MAX_TICKERS]);` — returns chunk count (== `parts`), `1..MAX_TICKERS`, or `0` on failure; fills `group_start[g]` with the first-row index of chunk `g`.
  - `size_t hub_build_report_frame(const ticker_runtime_t* rows, int lo, int hi, int part, int parts, char* buf, size_t cap);` — serializes `rows[lo..hi)` as one newline-terminated `cmd:"report"` frame; returns bytes (incl. `'\n'`, excl. NUL) or `0`.

- [ ] **Step 1: Write the failing tests**

Create `firmware/test/test_hub_report/test_main.cpp`:

```c
#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "core/hub_proto.h"

void setUp(void) {}
void tearDown(void) {}

static ticker_runtime_t mkrow(const char* id, ticker_source_t src, const char* sym,
                              const char* name, ticker_kind_t kind, uint16_t cad,
                              uint32_t stale, change_basis_t basis) {
  ticker_runtime_t r; memset(&r, 0, sizeof(r));
  strncpy(r.id, id, FIN_ID_LEN - 1);
  r.source = src;
  strncpy(r.symbol, sym, TKR_SYM_LEN - 1);
  strncpy(r.name, name, TKR_NAME_LEN - 1);
  r.kind = kind; r.cadence_s = cad; r.stale_s = stale; r.change_basis = basis;
  return r;
}

// ===== single-chunk frame shape =====
static void test_single_chunk_shape(void) {
  ticker_runtime_t rows[2] = {
    mkrow("bz_btcusdt", SRC_BINANCE, "BTCUSDT", "BTC", KIND_CRYPTO, 60, 600, CHG_24H),
    mkrow("yh_gspc", SRC_YAHOO, "%5EGSPC", "S&P 500", KIND_INDEX, 300, 600, CHG_PREV_CLOSE),
  };
  int gs[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(1, hub_report_plan(rows, 2, gs));   // both rows fit one chunk
  TEST_ASSERT_EQUAL_INT(0, gs[0]);

  char buf[HUB_FRAME_MAX];
  size_t n = hub_build_report_frame(rows, 0, 2, 0, 1, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);                 // newline-terminated

  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, buf));   // DeserializationError has operator bool, no int cast
  TEST_ASSERT_EQUAL_INT(1, doc["v"].as<int>());
  TEST_ASSERT_EQUAL_STRING("report", doc["cmd"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("tickers", doc["what"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(0, doc["rev"].as<int>());           // rev always 0
  TEST_ASSERT_EQUAL_INT(0, doc["part"].as<int>());
  TEST_ASSERT_EQUAL_INT(1, doc["parts"].as<int>());

  JsonArrayConst t = doc["tickers"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL_INT(2, (int)t.size());
  TEST_ASSERT_EQUAL_STRING("bz_btcusdt", t[0]["id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("binance",    t[0]["src"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("BTCUSDT",    t[0]["sym"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("BTC",        t[0]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("crypto",     t[0]["kind"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(60,  t[0]["cadence"].as<int>());
  TEST_ASSERT_EQUAL_INT(600, t[0]["stale"].as<int>());
  TEST_ASSERT_EQUAL_STRING("24h",        t[0]["basis"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("yahoo",      t[1]["src"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("%5EGSPC",    t[1]["sym"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("index",      t[1]["kind"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("prev_close", t[1]["basis"].as<const char*>());
}

// ===== enum -> wire mapping matrix =====
static void test_enum_to_wire(void) {
  struct { ticker_kind_t k; const char* s; } kinds[] = {
    {KIND_FX, "fx"}, {KIND_CRYPTO, "crypto"}, {KIND_INDEX, "index"}, {KIND_ETF, "etf"},
  };
  for (int i = 0; i < 4; i++) {
    ticker_runtime_t r = mkrow("id", SRC_YAHOO, "X", "X", kinds[i].k, 300, 600, CHG_PREV_CLOSE);
    char buf[HUB_FRAME_MAX];
    size_t n = hub_build_report_frame(&r, 0, 1, 0, 1, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    JsonDocument doc; deserializeJson(doc, buf);
    TEST_ASSERT_EQUAL_STRING(kinds[i].s, doc["tickers"][0]["kind"].as<const char*>());
  }
}

// ===== multi-chunk split (whole rows only) =====
static void test_multi_chunk_split(void) {
  // 16 rows with long names/symbols => must span >1 chunk under the 900B budget.
  ticker_runtime_t rows[MAX_TICKERS];
  for (int i = 0; i < MAX_TICKERS; i++)
    rows[i] = mkrow("idididididid", SRC_YAHOO, "SYMSYMSYMSYMSYMSYMSYM=X",
                    "LONGNAME LONGNAME NAME", KIND_INDEX, 300, 600, CHG_PREV_CLOSE);
  int gs[MAX_TICKERS];
  int parts = hub_report_plan(rows, MAX_TICKERS, gs);
  TEST_ASSERT_TRUE(parts >= 2);                              // genuinely chunked
  TEST_ASSERT_EQUAL_INT(0, gs[0]);
  for (int g = 1; g < parts; g++) TEST_ASSERT_TRUE(gs[g] > gs[g - 1]);   // strictly increasing

  // Every chunk serializes within budget and carries the right part/parts.
  for (int g = 0; g < parts; g++) {
    int lo = gs[g];
    int hi = (g + 1 < parts) ? gs[g + 1] : MAX_TICKERS;
    char buf[HUB_FRAME_MAX];
    size_t n = hub_build_report_frame(rows, lo, hi, g, parts, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n <= 900);
    JsonDocument doc; deserializeJson(doc, buf);
    TEST_ASSERT_EQUAL_INT(g, doc["part"].as<int>());
    TEST_ASSERT_EQUAL_INT(parts, doc["parts"].as<int>());
  }
}

// ===== count guards =====
static void test_count_guards(void) {
  ticker_runtime_t r = mkrow("a", SRC_YAHOO, "A", "A", KIND_FX, 300, 600, CHG_PREV_CLOSE);
  int gs[MAX_TICKERS];
  TEST_ASSERT_EQUAL_INT(0, hub_report_plan(&r, 0, gs));                 // count < 1
  TEST_ASSERT_EQUAL_INT(0, hub_report_plan(&r, MAX_TICKERS + 1, gs));   // count > MAX_TICKERS
  TEST_ASSERT_EQUAL_INT(0, hub_report_plan(NULL, 1, gs));               // null rows
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_chunk_shape);
  RUN_TEST(test_enum_to_wire);
  RUN_TEST(test_multi_chunk_split);
  RUN_TEST(test_count_guards);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_hub_report*"`
Expected: FAIL — link error / `hub_report_plan` + `hub_build_report_frame` undefined.

- [ ] **Step 3: Declare the functions in the header**

In `firmware/src/core/hub_proto.h`, after the `hub_build_config_ack` declaration (`:119`), before the `#ifdef __cplusplus` closer (`:121`):

```c
// --- Outbound: device -> hub ticker report (chunked running-table snapshot, issue #105) ---
// Pure, host-testable. The report mirrors the config ROW schema but rides the cmd channel:
// {"v":1,"cmd":"report","what":"tickers","rev":0,"part":P,"parts":N,"tickers":[{...row...}]}.
// rev is always 0 (the device does not persist the hub's rev). Split plan/frame so the caller
// serializes+sends one HUB_FRAME_MAX frame at a time (materializing all chunks would blow the
// hub task's 8 KB stack).

// Greedy whole-row chunk planner. Fills group_start[g] with the first-row index of chunk g and
// returns the chunk count (== parts), 1..MAX_TICKERS. Returns 0 on failure: count < 1 or
// > MAX_TICKERS, a null arg, an unmappable enum, or a single row that alone exceeds the ~900 B budget.
int hub_report_plan(const ticker_runtime_t* rows, int count, int group_start[MAX_TICKERS]);

// Serialize rows[lo..hi) as one newline-terminated cmd:"report" frame (part `part` of `parts`) into
// buf. Returns bytes (incl. '\n', excl. NUL), or 0 on overflow / unmappable enum / bad range.
size_t hub_build_report_frame(const ticker_runtime_t* rows, int lo, int hi,
                              int part, int parts, char* buf, size_t cap);
```

- [ ] **Step 4: Implement in the .cpp**

In `firmware/src/core/hub_proto.cpp`, append after `hub_build_config_ack` (after `:280`):

```c
// --- device -> hub ticker report (issue #105) ---
#define REPORT_CHUNK_MAX 900   // mirror ConfigFrame maxBytes; margin under HUB_FRAME_MAX

static const char* src_to_wire(ticker_source_t s) {
  switch (s) { case SRC_BINANCE: return "binance"; case SRC_YAHOO: return "yahoo"; }
  return NULL;
}
static const char* kind_to_wire(ticker_kind_t k) {
  switch (k) {
    case KIND_FX: return "fx"; case KIND_CRYPTO: return "crypto";
    case KIND_INDEX: return "index"; case KIND_ETF: return "etf";
  }
  return NULL;
}
static const char* basis_to_wire(change_basis_t b) {
  switch (b) { case CHG_PREV_CLOSE: return "prev_close"; case CHG_24H: return "24h"; }
  return NULL;
}

size_t hub_build_report_frame(const ticker_runtime_t* rows, int lo, int hi,
                              int part, int parts, char* buf, size_t cap) {
  if (!rows || !buf || cap == 0) return 0;
  if (parts <= 0 || part < 0 || part >= parts) return 0;            // bad chunk coordinates
  if (lo < 0 || hi <= lo || hi > MAX_TICKERS) return 0;             // bad / out-of-bounds range
  JsonDocument doc;
  doc["v"]    = 1;
  doc["cmd"]  = "report";
  doc["what"] = "tickers";
  doc["rev"]  = 0;
  doc["part"] = part;
  doc["parts"] = parts;
  JsonArray arr = doc["tickers"].to<JsonArray>();
  for (int i = lo; i < hi; i++) {
    const char* src   = src_to_wire(rows[i].source);
    const char* kind  = kind_to_wire(rows[i].kind);
    const char* basis = basis_to_wire(rows[i].change_basis);
    if (!src || !kind || !basis) return 0;          // unmappable enum => fail closed (all-or-zero)
    JsonObject o = arr.add<JsonObject>();
    o["id"]      = rows[i].id;
    o["src"]     = src;
    o["sym"]     = rows[i].symbol;
    o["name"]    = rows[i].name;
    o["kind"]    = kind;
    o["cadence"] = rows[i].cadence_s;
    o["stale"]   = rows[i].stale_s;
    o["basis"]   = basis;
  }
  return finish_frame(doc, buf, cap);
}

int hub_report_plan(const ticker_runtime_t* rows, int count, int group_start[MAX_TICKERS]) {
  if (!rows || !group_start || count < 1 || count > MAX_TICKERS) return 0;
  // Measure with worst-case header digits: parts=count AND part=count-1 (the real part/parts are both
  // <= these, so a group that fits the measurement still fits when re-serialized with the real values).
  const int wp = count - 1;
  int groups = 0, lo = 0;
  for (int i = 0; i < count; i++) {
    char tmp[HUB_FRAME_MAX];
    size_t n = hub_build_report_frame(rows, lo, i + 1, wp, count, tmp, sizeof(tmp));
    // n==0 when the probe overflowed HUB_FRAME_MAX (so definitely > REPORT_CHUNK_MAX) OR an enum error.
    // Both => "over budget"; the i==lo / n2==0 guards below still abort on a genuine single-row failure.
    bool over = (n == 0 || n > REPORT_CHUNK_MAX);
    if (!over) continue;                            // row i still fits the current group
    if (i == lo) return 0;                          // a single row alone exceeds the budget (or enum error)
    group_start[groups++] = lo;                     // close [lo..i)
    lo = i;                                          // row i opens a new group
    size_t n2 = hub_build_report_frame(rows, lo, i + 1, wp, count, tmp, sizeof(tmp));
    if (n2 == 0 || n2 > REPORT_CHUNK_MAX) return 0; // row i alone over budget
  }
  group_start[groups++] = lo;                       // final open group
  return groups;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `~/.beacon-pio/bin/pio test -e native -f "*test_hub_report*"`
Expected: PASS (4 tests).

- [ ] **Step 6: Run the full native suite (no regressions)**

Run: `~/.beacon-pio/bin/pio test -e native`
Expected: PASS — all suites, including `test_hub_config`.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/core/hub_proto.h firmware/src/core/hub_proto.cpp firmware/test/test_hub_report
git commit -m "feat(firmware): ticker report serializer (plan + frame) for issue #105"
```

---

### Task 2: Firmware emit gate in `hub_task.cpp`

Wire the serializer to BLE: emit the report once per connection, gated at the top of `on_frame`, reset on the disconnect edge. Device-only (`g_link`), so it is verified by compile + serial, not a host test.

**Files:**
- Modify: `firmware/src/core/hub_task.cpp` (add `s_reported` near `:26`; `send_ticker_report` after `send_config_ack` at `:40`; emit at the top of `on_frame` `:89`; reset at the disconnect edge `:135`)

**Interfaces:**
- Consumes: `hub_report_plan`, `hub_build_report_frame` (Task 1); `ticker_table_count` / `ticker_table_get` (`config/ticker_table.h`); `g_link->send` (existing).
- Produces: nothing for later tasks (internal wiring).

- [ ] **Step 1: Add the per-connection flag**

In `firmware/src/core/hub_task.cpp`, alongside the other file-scope statics (`:24-30`), add:

```c
static bool       s_reported = false;   // emitted the once-per-connection ticker report? (issue #105)
```

- [ ] **Step 2: Add the emit helper**

After `send_config_ack` (`:40`), add:

```c
// Emit the device's current ticker table to the hub as chunked cmd:"report" frames (issue #105), so a
// fresh hub can adopt the list it already holds. One-way (no ack). Plans once, then serializes+sends one
// HUB_FRAME_MAX frame at a time. Returns true only if EVERY chunk was accepted by the link: a mid-stream
// send failure (queue full) returns false so the caller does NOT latch s_reported and retries on the next
// inbound frame this connection -- a retry re-sends part 0, which restarts the hub accumulator (the hub
// adopts only on the final part, so a partial delivery never half-adopts). g_link null (native) => true.
static bool send_ticker_report(void) {
  if (!g_link) return true;
  int count = ticker_table_count();
  if (count > MAX_TICKERS) count = MAX_TICKERS;
  ticker_runtime_t rows[MAX_TICKERS];
  for (int i = 0; i < count; i++) {
    if (!ticker_table_get(i, &rows[i])) return false;  // table shrank under us => retry next frame
  }
  int gs[MAX_TICKERS];
  int parts = hub_report_plan(rows, count, gs);
  if (parts < 1) return true;                          // nothing emittable (e.g. unmappable enum) => don't spin
  char buf[HUB_FRAME_MAX];
  for (int p = 0; p < parts; p++) {
    int lo = gs[p];
    int hi = (p + 1 < parts) ? gs[p + 1] : count;
    size_t n = hub_build_report_frame(rows, lo, hi, p, parts, buf, sizeof(buf));
    if (!n || !g_link->send(buf, n)) return false;     // serialize/send failed => retry whole report later
  }
  LOGI("hub: ticker report sent (%d rows, %d parts)", count, parts);
  return true;
}
```

- [ ] **Step 3: Emit at the top of `on_frame`**

In `on_frame` (`:89`), insert as the FIRST statements of the function body, before the ack `frame_has` check (`:90`):

```c
  if (!s_reported) {                 // first inbound frame this connection proves the central is listening
    s_reported = send_ticker_report();   // latch only on full success; emit BEFORE dispatch (on_frame has
                                         // early returns for ack/config below). A failed send retries next frame.
  }
```

- [ ] **Step 4: Reset on the disconnect edge**

In `hub_task` loop, at the existing edge-trigger (`:135`):

```c
    if (s_was_connected && !c) { ds_set_hub_offline(); s_reported = false; }   // re-report next connection
```

(Replaces the current `if (s_was_connected && !c) ds_set_hub_offline();`.)

- [ ] **Step 5: Compile for the device**

Run: `cd firmware && ~/.beacon-pio/bin/pio run`
Expected: SUCCESS — links `hub_report_plan` / `hub_build_report_frame` into the firmware image.

- [ ] **Step 6: Run the native suite (guard nothing broke host-side)**

Run: `~/.beacon-pio/bin/pio test -e native`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/core/hub_task.cpp
git commit -m "feat(firmware): emit ticker report once per connection (issue #105)"
```

> Manual device verification (deferred to end-to-end after Task 5): flash, connect a fresh hub, confirm the serial log prints `hub: ticker report sent` and the hub panel fills.

---

### Task 3: Hub parse — `DeviceCommand.report`

Add a per-chunk `report` case to the Swift command parser, reusing `TickerRow` decoding.

**Files:**
- Modify: `hub/Sources/BeaconHubKit/Protocol.swift` (`DeviceCommand` enum + `parse`, `:88-108`)
- Modify: `hub/Tests/BeaconHubKitTests/ProtocolTests.swift` (add report cases)

**Interfaces:**
- Consumes: `TickerRow`, `TickerSource`, `TickerKind`, `ChangeBasis` (`TickerConfig.swift`).
- Produces: `case report(what: String, rev: UInt32, part: Int, parts: Int, rows: [TickerRow])` on `DeviceCommand`.

- [ ] **Step 1: Write the failing tests**

In `hub/Tests/BeaconHubKitTests/ProtocolTests.swift`, add inside the test class:

```swift
func testParseReportChunk() {
    let json = #"""
    {"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":2,"tickers":[\#
    {"id":"bz_btcusdt","src":"binance","sym":"BTCUSDT","name":"BTC","kind":"crypto","cadence":60,"stale":600,"basis":"24h"}]}
    """#
    guard case let .report(what, rev, part, parts, rows) = DeviceCommand.parse(Data(json.utf8)) else {
        return XCTFail("expected .report")
    }
    XCTAssertEqual(what, "tickers")
    XCTAssertEqual(rev, 0)
    XCTAssertEqual(part, 0)
    XCTAssertEqual(parts, 2)
    XCTAssertEqual(rows.count, 1)
    XCTAssertEqual(rows[0], TickerRow(id: "bz_btcusdt", src: .binance, sym: "BTCUSDT", name: "BTC",
                                      kind: .crypto, cadence: 60, stale: 600, basis: .h24))
}

func testParseReportRejectsBadWhatOrParts() {
    // unknown `what`
    XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"report","what":"weather","rev":0,"part":0,"parts":1,"tickers":[]}"#.utf8)))
    // part out of range
    XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"report","what":"tickers","rev":0,"part":2,"parts":2,"tickers":[]}"#.utf8)))
    // parts <= 0
    XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":0,"tickers":[]}"#.utf8)))
    // malformed row (bad enum)
    XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":1,"tickers":[{"id":"x","src":"nope","sym":"X","name":"X","kind":"fx","cadence":1,"stale":1,"basis":"24h"}]}"#.utf8)))
    // empty id
    XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":1,"tickers":[{"id":"","src":"yahoo","sym":"X","name":"X","kind":"fx","cadence":1,"stale":1,"basis":"24h"}]}"#.utf8)))
    // id over 15-byte cap
    let longId = String(repeating: "z", count: 16)
    XCTAssertNil(DeviceCommand.parse(Data(#"{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":1,"tickers":[{"id":"\#(longId)","src":"yahoo","sym":"X","name":"X","kind":"fx","cadence":1,"stale":1,"basis":"24h"}]}"#.utf8)))
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd hub && swift test --filter ProtocolTests`
Expected: FAIL — `.report` is not a member of `DeviceCommand`.

- [ ] **Step 3: Add the enum case + parse**

In `hub/Sources/BeaconHubKit/Protocol.swift`, add the case to `DeviceCommand` (after `:92`):

```swift
    // Device-initiated snapshot of its running ticker list (issue #105). Per-chunk; the caller
    // reassembles. rev is always 0 (the device does not persist the hub's rev). Used so a fresh hub
    // can adopt the list the device already holds.
    case report(what: String, rev: UInt32, part: Int, parts: Int, rows: [TickerRow])
```

In `parse`, add a `case "report":` before `default:` (`:104`):

```swift
        case "report":
            guard (obj["what"] as? String) == "tickers",
                  let rev = obj["rev"] as? Int, rev >= 0,
                  let part = obj["part"] as? Int, let parts = obj["parts"] as? Int,
                  parts > 0, part >= 0, part < parts,
                  let arr = obj["tickers"] as? [[String: Any]] else { return nil }
            var rows = [TickerRow]()
            for r in arr {
                guard let id = r["id"] as? String, !id.isEmpty,
                      id.utf8.count <= TickerLimits.idMaxBytes,
                      let src = (r["src"] as? String).flatMap(TickerSource.init(rawValue:)),
                      let sym = r["sym"] as? String, sym.utf8.count <= TickerLimits.symMaxBytes,
                      let name = r["name"] as? String, name.utf8.count <= TickerLimits.nameMaxBytes,
                      let kind = (r["kind"] as? String).flatMap(TickerKind.init(rawValue:)),
                      let cadence = r["cadence"] as? Int, let stale = r["stale"] as? Int,
                      let basis = (r["basis"] as? String).flatMap(ChangeBasis.init(rawValue:))
                else { return nil }   // any malformed / over-cap row drops the whole chunk (parity with config)
                rows.append(TickerRow(id: id, src: src, sym: sym, name: name,
                                      kind: kind, cadence: cadence, stale: stale, basis: basis))
            }
            return .report(what: "tickers", rev: UInt32(rev), part: part, parts: parts, rows: rows)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd hub && swift test --filter ProtocolTests`
Expected: PASS (including the two new cases).

- [ ] **Step 5: Commit**

```bash
git add hub/Sources/BeaconHubKit/Protocol.swift hub/Tests/BeaconHubKitTests/ProtocolTests.swift
git commit -m "feat(hub): parse device->hub cmd:report frame (issue #105)"
```

---

### Task 4: Hub reassembler + `isPristine`

Pure per-connection chunk reassembler in `BeaconHubKit` and a pristine-store predicate, both host-tested.

**Files:**
- Create: `hub/Sources/BeaconHubKit/ReportAssembler.swift`
- Modify: `hub/Sources/BeaconHubKit/TickerConfigState.swift` (add `isPristine`)
- Create: `hub/Tests/BeaconHubKitTests/ReportAssemblerTests.swift`

**Interfaces:**
- Consumes: `DeviceCommand.report` (Task 3), `TickerRow`, `TickerConfigState`.
- Produces:
  - `struct ReportAssembler { mutating func feed(_ cmd: DeviceCommand) -> ReportResult; mutating func reset() }`
  - `enum ReportResult: Equatable { case pending, dropped, assembled([TickerRow]) }`
  - `extension TickerConfigState { var isPristine: Bool }`

- [ ] **Step 1: Write the failing tests**

Create `hub/Tests/BeaconHubKitTests/ReportAssemblerTests.swift`:

```swift
import XCTest
@testable import BeaconHubKit

final class ReportAssemblerTests: XCTestCase {
    private func chunk(_ part: Int, _ parts: Int, _ rows: [TickerRow], rev: UInt32 = 0) -> DeviceCommand {
        .report(what: "tickers", rev: rev, part: part, parts: parts, rows: rows)
    }
    private func row(_ id: String) -> TickerRow {
        TickerRow(id: id, src: .binance, sym: "BTCUSDT", name: "BTC",
                  kind: .crypto, cadence: 60, stale: 600, basis: .h24)
    }

    func testSingleChunkAssembles() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 1, [row("a")])), .assembled([row("a")]))
    }

    func testMultiChunkAssemblesInOrder() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")])), .assembled([row("a"), row("b")]))
    }

    func testGapDrops() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 3, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(2, 3, [row("c")])), .dropped)   // skipped part 1
    }

    func testPartZeroRestarts() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)   // part 0 restarts (not a fault)
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")])), .assembled([row("a"), row("b")]))
    }

    func testPartsMismatchDrops() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        XCTAssertEqual(a.feed(chunk(1, 3, [row("b")])), .dropped)   // parts changed mid-stream
    }

    func testRevMismatchDrops() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")], rev: 0)), .pending)
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")], rev: 1)), .dropped)   // rev changed mid-stream
    }

    func testResetClearsPartial() {
        var a = ReportAssembler()
        XCTAssertEqual(a.feed(chunk(0, 2, [row("a")])), .pending)
        a.reset()
        XCTAssertEqual(a.feed(chunk(1, 2, [row("b")])), .dropped)   // no active accumulation after reset
    }

    func testIsPristine() {
        XCTAssertTrue(TickerConfigState().isPristine)                              // rev 0, empty
        XCTAssertFalse(TickerConfigState(rows: [row("a")], rev: 0).isPristine)     // has rows
        XCTAssertFalse(TickerConfigState(rows: [], rev: 1).isPristine)             // user emptied (rev>0)
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd hub && swift test --filter ReportAssemblerTests`
Expected: FAIL — `ReportAssembler` / `ReportResult` / `isPristine` not defined.

- [ ] **Step 3: Implement `ReportAssembler`**

Create `hub/Sources/BeaconHubKit/ReportAssembler.swift`:

```swift
import Foundation

// Per-connection reassembler for the device -> hub cmd:"report" chunks (issue #105). Mirrors the
// firmware config_accum_t: part 0 (re)starts; later parts must be contiguous and share `parts`. Any
// gap / duplicate-past-0 / parts-mismatch discards the partial and returns .dropped (fail-closed). The
// owner resets it on the disconnect edge. Pure + host-testable; non-report commands return .pending
// (ignored, no effect on an in-progress accumulation).
public enum ReportResult: Equatable {
    case pending
    case dropped
    case assembled([TickerRow])
}

public struct ReportAssembler {
    private var rev: UInt32 = 0
    private var parts = 0
    private var nextPart = 0
    private var rows = [TickerRow]()
    private var active = false

    public init() {}

    public mutating func reset() {
        rev = 0; parts = 0; nextPart = 0; rows.removeAll(); active = false
    }

    public mutating func feed(_ cmd: DeviceCommand) -> ReportResult {
        guard case let .report(_, rev, part, parts, chunkRows) = cmd else { return .pending }

        if part == 0 {                                  // part 0 always (re)starts a fresh accumulation
            active = true; self.rev = rev; self.parts = parts; nextPart = 0; rows.removeAll()
        } else if !active || rev != self.rev || parts != self.parts || part != nextPart {
            reset()                                     // out-of-order / rev or parts mismatch / no active run
            return .dropped
        }

        rows.append(contentsOf: chunkRows)
        nextPart = part + 1
        guard nextPart >= self.parts else { return .pending }

        let assembled = rows
        reset()
        return .assembled(assembled)
    }
}
```

- [ ] **Step 4: Add `isPristine`**

In `hub/Sources/BeaconHubKit/TickerConfigState.swift`, inside the struct (after `updating`, before the closing brace at `:20`):

```swift
    // A never-configured store: still at the seed rev with no rows. A fresh hub adopts a device report
    // only when pristine; once the user has edited (rev > 0) the hub stays the source of truth (#105).
    public var isPristine: Bool { rev == 0 && rows.isEmpty }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd hub && swift test --filter ReportAssemblerTests`
Expected: PASS (8 tests).

- [ ] **Step 6: Commit**

```bash
git add hub/Sources/BeaconHubKit/ReportAssembler.swift hub/Sources/BeaconHubKit/TickerConfigState.swift hub/Tests/BeaconHubKitTests/ReportAssemblerTests.swift
git commit -m "feat(hub): report reassembler + pristine-store predicate (issue #105)"
```

---

### Task 5: Hub adoption wiring in `AppDelegate`

Hold a `ReportAssembler`, handle `.report` in the command dispatcher, adopt when pristine, and reset on disconnect. Thin glue; verified by `swift build` + the existing suite + manual end-to-end.

**Files:**
- Modify: `hub/Sources/beacon-hub/AppDelegate.swift` (property near `tickerStore` `:18`; `.report` case in `handle(_:)` `:235-254`; `adoptDeviceReport` helper near `pushTickerConfig` `:375`; assembler reset where the link drops)

**Interfaces:**
- Consumes: `ReportAssembler`, `ReportResult`, `DeviceCommand.report`, `TickerConfigState.isPristine` (Tasks 3-4); `tickerStore`, `menubar.setTickerRows`, `menubar.setTickerSync` (existing).
- Produces: nothing for later tasks.

- [ ] **Step 1: Add the assembler property**

In `hub/Sources/beacon-hub/AppDelegate.swift`, next to `tickerStore` (`:18`):

```swift
    private var reportAssembler = ReportAssembler()   // reassembles device->hub ticker report chunks (#105)
```

- [ ] **Step 2: Handle `.report` in `handle(_:)`**

In the `switch cmd` of `handle(_:)` (`:236`), add a case after `.configAck` (`:248-253`):

```swift
        case .report(_, _, _, _, _):
            switch reportAssembler.feed(cmd) {
            case .assembled(let rows): adoptDeviceReport(rows)
            case .pending, .dropped:   break
            }
```

- [ ] **Step 3: Add the adoption helper**

In `hub/Sources/beacon-hub/AppDelegate.swift`, after `pushTickerConfig()` (`:384`):

```swift
    // Adopt the device's reported ticker list on a fresh pairing (issue #105): only when our store is
    // pristine (rev 0, no rows) -- otherwise the hub stays the source of truth and its onReady push
    // reconciles the device. Persist (rev 0 -> 1) + refresh the panel; do NOT push back (the device
    // already has this list -- pushing would be a pointless echo).
    private func adoptDeviceReport(_ rows: [TickerRow]) {
        guard tickerStore.current.isPristine else { return }
        tickerStore.save(rows: rows)
        menubar.setTickerRows(tickerStore.current.rows)
        menubar.setTickerSync(.synced(tickerStore.current.rows.count))
    }
```

- [ ] **Step 4: Reset the assembler at the start of each connection**

`BeaconCentral` has no plain `.disconnected` phase callback (only `.reconnecting` / `.pairingFailed`), so reset at connection start instead — `central.onReady` (`AppDelegate.swift:187-195`) fires once per fresh (re)subscription. A reset here is equally fail-closed: any partial from a dropped prior connection is discarded before the new connection's `part == 0` arrives. Add as the first line of the `onReady` closure body, before `sendFullFrame`:

```swift
            self?.reportAssembler.reset()   // discard any partial device report from a prior connection (#105)
```

- [ ] **Step 5: Build and run the hub suite**

Run: `cd hub && swift build && swift test`
Expected: SUCCESS — compiles; all `BeaconHubKit` tests pass.

- [ ] **Step 6: Commit**

```bash
git add hub/Sources/beacon-hub/AppDelegate.swift
git commit -m "feat(hub): adopt device ticker report on fresh pairing (issue #105)"
```

---

### Task 6: Contract §B3 + end-to-end verification

Record the frozen-style contract section and a shared fixture, then run the full end-to-end check.

**Files:**
- Modify: `hub/CONTRACT.md` (add §B3 after §B2, before §C at `:103`)

**Interfaces:** none (documentation + verification).

- [ ] **Step 1: Add §B3 to CONTRACT.md**

Insert after §B2 (before `## C.` at `:103`):

```markdown
## B3. Device -> hub ticker report (additive, issue #105, design `docs/specs/2026-06-19-device-ticker-report-design.md`)

So a fresh (never-configured) hub adopts the list the device already holds, the device emits a one-way
`report` on the device->hub `cmd` channel, **once per connection** after the first inbound hub frame.
Full rows, chunked exactly like §B2 `config` (same row schema/caps), but the envelope is flat `cmd`
fields (not a nested `config` object). The hub adopts only when its store is pristine
(`rev == 0 && rows.isEmpty`); otherwise it ignores the report and stays the source of truth. Mirror of
`firmware/.../hub_proto.cpp` (`hub_report_plan` / `hub_build_report_frame`) and
`BeaconHubKit/Protocol.swift` (`DeviceCommand.report`) + `ReportAssembler`.

```json
{"v":1,"cmd":"report","what":"tickers","rev":0,"part":0,"parts":2,"tickers":[
  {"id":"ygspc","src":"yahoo","sym":"%5EGSPC","name":"S&P 500","kind":"index","cadence":300,"stale":600,"basis":"prev_close"}]}
{"v":1,"cmd":"report","what":"tickers","rev":0,"part":1,"parts":2,"tickers":[
  {"id":"bbtcusdt","src":"binance","sym":"BTCUSDT","name":"BTC","kind":"crypto","cadence":60,"stale":600,"basis":"24h"}]}
```

- `cmd` = `"report"`; `what` = `"tickers"` (namespaces the verb; the hub ignores any other `what`).
- `rev` is **always `0`** -- the device does not persist the hub's rev, and the hub never uses the
  reported value (it adopts its own pristine `rev 0 -> 1`). Carried for structural symmetry + chunk
  continuity only.
- `part` / `parts`, chunking budget, and row keys/caps are **identical to §B2** (rows concatenated in
  `part` order == display order; line <= ~900 B; row never split; <= 16 rows; trailing `0x0A`).
- **No ack.** One-way and informational. An older hub that does not know `cmd:"report"` drops it
  (`DeviceCommand.parse` returns `nil` on an unknown `cmd`).
```

- [ ] **Step 2: Run both full suites**

Run: `~/.beacon-pio/bin/pio test -e native`
Expected: PASS — all firmware suites.

Run: `cd hub && swift test`
Expected: PASS — all `BeaconHubKit` suites.

- [ ] **Step 3: Manual end-to-end (real device + hub)**

1. Configure a non-default ticker list on the device from hub A; confirm the device persists it.
2. On a machine with no `BeaconTickerConfig` UserDefaults (or after `defaults delete <hub-bundle-id> BeaconTickerConfig`), launch hub B and pair the device.
3. Confirm: the device serial logs `hub: ticker report sent (N rows, P parts)`; hub B's Tickers panel shows the device's list (not `0/16`) and sync shows `synced`.
4. Edit one ticker on hub B; confirm the normal `config` push path still works (panel updates, device re-syncs, `config_ack`).

- [ ] **Step 4: Commit**

```bash
git add hub/CONTRACT.md
git commit -m "docs(hub): contract §B3 device->hub ticker report (issue #105)"
```

---

## Self-Review

**Spec coverage:**
- Device -> hub `report` frame, chunked, full rows => Task 1 (serializer) + Task 2 (emit).
- Emit once per connection after first inbound frame => Task 2 (`s_reported` gate at top of `on_frame`, reset on disconnect).
- Hub adopts only when pristine; persist rev 0->1, refresh panel, mark synced, no push-back => Task 4 (`isPristine`) + Task 5 (`adoptDeviceReport`).
- Backward-compat / old hub drops unknown cmd => Task 3 (`parse` default `nil`) + asserted in `testParseRejectsBadVersionOrCmd` (existing) and §B3.
- Malformed/incomplete report fail-closed => Task 3 (per-row `nil`) + Task 4 (`.dropped`).
- Contract kept in sync (CONTRACT.md, Protocol.swift, hub_proto.cpp + tests) => Tasks 1/3/6.
- Non-goals (merge, re-adopt after emptied, per-device lists, NVS rev) => not implemented by design; `isPristine` enforces the emptied-list non-goal (Task 4 `testIsPristine`).

**Placeholder scan:** none — every code/test step shows full content; the only judgement call (Task 5 Step 4 disconnect hook location) is bounded with an explicit fallback.

**Type consistency:** `hub_report_plan` / `hub_build_report_frame` signatures match across Tasks 1-2; `DeviceCommand.report(what:rev:part:parts:rows:)` identical in Tasks 3-5; `ReportResult` / `ReportAssembler.feed` / `reset` / `isPristine` identical in Tasks 4-5; `setTickerSync(.synced(Int))` matches `TickerSyncStatus` (`HubViewModel.swift:11`).
