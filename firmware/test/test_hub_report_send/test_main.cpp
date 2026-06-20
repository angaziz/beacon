#include <unity.h>
#include <string>
#include <cstring>
#include "core/hub_report.h"
#include "core/hublink.h"

// Regression for issue #106: send_ticker_report (via hub_emit_report) must deliver EVERY chunk of a large
// ticker list even though the whole report exceeds the link's bounded, all-or-nothing send buffer. The fix
// is the per-chunk flush() that drains the buffer before the next enqueue. These tests pin both halves:
// with flush the full report lands; without flush (the pre-fix behaviour) a later chunk is rejected.

// A HubLink that models s_out: send() copies into a fixed-capacity buffer and is all-or-nothing (rejects
// when the frame would not fit the remaining space); flush() "transmits" the buffered bytes (appends them
// to `delivered`) and empties it. CAP is sized so one chunk fits but two do not -- exactly the s_out edge.
class BoundedHub : public HubLink {
public:
  static const int CAP = 1024;   // < two report chunks, > one (REPORT_CHUNK_MAX = 900)
  bool drains;                   // false reproduces the pre-#106 bug (flush is a no-op)
  char buf[CAP];
  int used = 0;
  std::string delivered;
  int sends = 0, flushes = 0;

  explicit BoundedHub(bool drains_) : drains(drains_) {}

  bool begin() override { return true; }
  bool isConnected() override { return true; }
  void onFrame(hub_frame_cb) override {}
  bool send(const char* json, size_t len) override {
    if ((int)len > CAP - used) return false;   // all-or-nothing against the bounded buffer
    memcpy(buf + used, json, len);
    used += (int)len;
    sends++;
    return true;
  }
  void flush() override {
    flushes++;
    if (!drains) return;                        // pre-fix: buffer never drains between sends
    delivered.append(buf, (size_t)used);
    used = 0;
  }
  void loop() override {}

  int deliveredRowCount() const {              // count "id": occurrences == rows that reached the hub
    int n = 0;
    for (size_t i = delivered.find("\"id\":"); i != std::string::npos;
         i = delivered.find("\"id\":", i + 1)) n++;
    return n;
  }
};

// 16 max-length rows so the report needs several ~800 B chunks whose total far exceeds CAP.
static int buildMaxRows(ticker_runtime_t* rows) {
  for (int i = 0; i < MAX_TICKERS; i++) {
    ticker_runtime_t* r = &rows[i];
    memset(r, 0, sizeof(*r));
    snprintf(r->id, FIN_ID_LEN, "id%012d", i);                 // fill the id field
    memset(r->symbol, 'S', TKR_SYM_LEN - 1); r->symbol[0] = (char)('A' + i); r->symbol[TKR_SYM_LEN - 1] = 0;
    memset(r->name, 'N', TKR_NAME_LEN - 1);  r->name[0] = (char)('A' + i);  r->name[TKR_NAME_LEN - 1] = 0;
    r->source = SRC_YAHOO;
    r->kind = KIND_INDEX;
    r->change_basis = CHG_PREV_CLOSE;
    r->cadence_s = 300;
    r->stale_s = 600;
  }
  return MAX_TICKERS;
}

void setUp(void) {}
void tearDown(void) {}

// With the per-chunk flush, the full 16-row report is delivered despite exceeding the bounded buffer.
static void test_large_report_delivered_with_flush(void) {
  ticker_runtime_t rows[MAX_TICKERS];
  int count = buildMaxRows(rows);
  BoundedHub hub(/*drains=*/true);

  int parts = hub_emit_report(&hub, rows, count);

  TEST_ASSERT_TRUE_MESSAGE(parts >= 2, "test setup must produce a multi-chunk report");
  TEST_ASSERT_EQUAL_INT_MESSAGE(count, hub.deliveredRowCount(), "every row must reach the hub");
  TEST_ASSERT_TRUE(hub.flushes >= parts);
}

// Without draining between chunks (the pre-#106 behaviour) a later chunk overruns the buffer: send()
// rejects it and hub_emit_report reports failure -- the regression this guards against.
static void test_large_report_fails_without_drain(void) {
  ticker_runtime_t rows[MAX_TICKERS];
  int count = buildMaxRows(rows);
  BoundedHub hub(/*drains=*/false);

  int parts = hub_emit_report(&hub, rows, count);

  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parts, "an undrained bounded buffer must reject a later chunk");
}

// A small list whose whole report fits in one chunk still works (single-part path, no flush dependency).
static void test_small_report_single_chunk(void) {
  ticker_runtime_t rows[2];
  for (int i = 0; i < 2; i++) {
    memset(&rows[i], 0, sizeof(rows[i]));
    snprintf(rows[i].id, FIN_ID_LEN, "btc%d", i);
    strncpy(rows[i].symbol, "BTCUSDT", TKR_SYM_LEN - 1);
    strncpy(rows[i].name, "BTC", TKR_NAME_LEN - 1);
    rows[i].source = SRC_BINANCE; rows[i].kind = KIND_CRYPTO; rows[i].change_basis = CHG_24H;
    rows[i].cadence_s = 60; rows[i].stale_s = 600;
  }
  BoundedHub hub(/*drains=*/true);

  int parts = hub_emit_report(&hub, rows, 2);

  TEST_ASSERT_EQUAL_INT(1, parts);
  TEST_ASSERT_EQUAL_INT(2, hub.deliveredRowCount());
}

// A null link is a no-op success (native / not-yet-started transport).
static void test_null_link_noop(void) {
  ticker_runtime_t rows[1];
  memset(&rows[0], 0, sizeof(rows[0]));
  TEST_ASSERT_EQUAL_INT(0, hub_emit_report(nullptr, rows, 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_large_report_delivered_with_flush);
  RUN_TEST(test_large_report_fails_without_drain);
  RUN_TEST(test_small_report_single_chunk);
  RUN_TEST(test_null_link_noop);
  return UNITY_END();
}
