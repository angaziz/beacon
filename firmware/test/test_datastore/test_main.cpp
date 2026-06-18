#include <unity.h>
#include <string.h>
#include "core/datastore.h"
#include "config/tickers.h"
#include "config/ticker_table.h"

// datastore_init() now seeds finance ids/count from the runtime ticker table (#92), so the table must
// be initialized first; ticker_table_init() seeds from DEFAULT_TICKERS on the native build (NVS stub).
void setUp(void) { ticker_table_init(); datastore_init(); }   // reset the static store before each test
void tearDown(void) {}

static void test_init_seeds_finance(void) {
  TEST_ASSERT_EQUAL_UINT8(DEFAULT_TICKERS_COUNT, ds_get_finance_count());
  finance_rec_t f0 = ds_get_finance(0);
  TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[0].id, f0.id);
  TEST_ASSERT_EQUAL_INT(ST_LOADING, f0.hdr.state);
}

static void test_weather_set_get_forces_live(void) {
  weather_rec_t w; memset(&w, 0, sizeof(w));
  w.temp_c = 30.5f; w.humidity_pct = 70; w.wmo_code = 2;
  w.hdr.last_updated = 1000; w.hdr.state = ST_ERROR;   // setter must override to LIVE
  ds_set_weather(&w);
  weather_rec_t g = ds_get_weather();
  TEST_ASSERT_EQUAL_FLOAT(30.5f, g.temp_c);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, g.hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_NONE, g.hdr.err);
}

static void test_finance_isolation_and_id_preserved(void) {
  finance_rec_t r; memset(&r, 0, sizeof(r));
  r.value = 123.0; r.hdr.last_updated = 1000;
  strcpy(r.id, "WRONG");                  // caller id ignored; slot keeps seeded id
  ds_set_finance(2, &r);
  finance_rec_t s2 = ds_get_finance(2);
  finance_rec_t s3 = ds_get_finance(3);
  TEST_ASSERT_EQUAL_FLOAT(123.0f, (float)s2.value);
  TEST_ASSERT_EQUAL_STRING(DEFAULT_TICKERS[2].id, s2.id);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, s2.hdr.state);
  TEST_ASSERT_EQUAL_INT(ST_LOADING, s3.hdr.state);   // neighbor untouched
}

static void test_staleness_inclusive_boundary(void) {
  struct { uint32_t last; uint32_t now; int expect; } cases[] = {
    {1000, 1000 + 1799, ST_LIVE},
    {1000, 1000 + 1800, ST_STALE},   // WEATHER_STALE_S, inclusive
    {1000, 1000 + 5000, ST_STALE},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    datastore_init();
    weather_rec_t w; memset(&w, 0, sizeof(w)); w.hdr.last_updated = cases[i].last;
    ds_set_weather(&w);                // LIVE
    ds_tick_staleness(cases[i].now);
    TEST_ASSERT_EQUAL_INT(cases[i].expect, ds_get_weather().hdr.state);
  }
}

static void test_sweep_never_clobbers_explicit_states(void) {
  ds_set_state_weather(ST_ERROR, ERR_RATE_LIMITED);
  ds_tick_staleness(9999999);
  TEST_ASSERT_EQUAL_INT(ST_ERROR, ds_get_weather().hdr.state);   // not promoted to STALE
  ds_set_hub_offline();
  ds_tick_staleness(9999999);
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_usage().hdr.state);
}

static void test_hub_offline_flip_and_recovery(void) {
  ds_set_hub_offline();
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_usage().hdr.state);
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, ds_get_buddy().hdr.state);
  usage_rec_t u; memset(&u, 0, sizeof(u)); u.claude.h5.pct = 24; u.hdr.last_updated = 1000;
  ds_set_usage(&u);
  TEST_ASSERT_EQUAL_INT(ST_LIVE, ds_get_usage().hdr.state);      // recovered
  TEST_ASSERT_EQUAL_INT16(24, ds_get_usage().claude.h5.pct);
}

static void test_explicit_failure_preserves_payload(void) {
  weather_rec_t w; memset(&w, 0, sizeof(w)); w.temp_c = 28.0f; w.hdr.last_updated = 1000;
  ds_set_weather(&w);
  ds_set_state_weather(ST_OFFLINE, ERR_NO_ROUTE);
  weather_rec_t g = ds_get_weather();
  TEST_ASSERT_EQUAL_INT(ST_OFFLINE, g.hdr.state);
  TEST_ASSERT_EQUAL_INT(ERR_NO_ROUTE, g.hdr.err);
  TEST_ASSERT_EQUAL_FLOAT(28.0f, g.temp_c);                      // value preserved
}

// ds_tick_buddy_prompt: state-preserving prompt lifecycle (monotonic uptime). `now`/stamps share the
// uptime epoch. The tick mutates prompt only, never hdr.state.
static void mk_prompt(uint8_t decision, uint32_t shown_at, uint32_t decided_at) {
  buddy_rec_t b; memset(&b, 0, sizeof(b));
  b.prompt.present = true;
  b.prompt.decision_state = decision;
  b.prompt.shown_at = shown_at;
  b.prompt.decided_at = decided_at;
  b.hdr.last_updated = 1000;
  ds_set_buddy(&b);   // forces ST_LIVE
}

static void test_tick_buddy_prompt_transitions(void) {
  struct {
    const char* name;
    uint8_t  decision;
    uint32_t shown_at, decided_at, now;
    bool     expect_present;
    uint8_t  expect_decision;
  } cases[] = {
    // IDLE prompt: expires to TOO_LATE at/after BUDDY_PROMPT_EXPIRY_S, else unchanged.
    {"idle_before_expiry", PROMPT_IDLE_DECISION, 100, 0, 100 + BUDDY_PROMPT_EXPIRY_S - 1, true, PROMPT_IDLE_DECISION},
    {"idle_at_expiry",     PROMPT_IDLE_DECISION, 100, 0, 100 + BUDDY_PROMPT_EXPIRY_S,     true, PROMPT_TOO_LATE},
    // SENT_OK beat: clears (present=false) at/after BUDDY_CONFIRM_HOLD_S (2), else held.
    {"sent_before_hold",   PROMPT_SENT_OK, 100, 200, 200 + 1, true,  PROMPT_SENT_OK},
    {"sent_at_hold",       PROMPT_SENT_OK, 100, 200, 200 + 2, false, PROMPT_SENT_OK},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    datastore_init();
    mk_prompt(cases[i].decision, cases[i].shown_at, cases[i].decided_at);
    ds_tick_buddy_prompt(cases[i].now);
    buddy_rec_t g = ds_get_buddy();
    TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].expect_present, g.prompt.present, cases[i].name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(cases[i].expect_decision, g.prompt.decision_state, cases[i].name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ST_LIVE, g.hdr.state, cases[i].name);   // tick never touches hdr.state
  }
}

static void test_tick_buddy_prompt_preserves_hub_offline(void) {
  mk_prompt(PROMPT_IDLE_DECISION, 100, 0);
  ds_set_hub_offline();                 // flips buddy hdr.state, leaves the prompt
  ds_tick_buddy_prompt(100 + BUDDY_PROMPT_EXPIRY_S + 100);      // well past expiry
  buddy_rec_t g = ds_get_buddy();
  TEST_ASSERT_EQUAL_INT(ST_HUB_OFFLINE, g.hdr.state);          // never erased by the tick
  TEST_ASSERT_EQUAL_UINT8(PROMPT_TOO_LATE, g.prompt.decision_state);   // prompt still ages
}

// A5 stale-fetch guard: ds_set_finance_if publishes only when the slot still holds the expected id.
static void test_set_finance_if_publish_vs_drop(void) {
  struct { const char* name; const char* expect_id; bool published; } cases[] = {
    {"match_publishes", nullptr,    true},    // expect_id filled from the seeded id below
    {"mismatch_drops",  "STALE_ID", false},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    datastore_init();
    finance_rec_t seeded = ds_get_finance(0);   // ST_LOADING, value 0
    const char* expect = cases[i].expect_id ? cases[i].expect_id : seeded.id;
    finance_rec_t r; memset(&r, 0, sizeof(r)); r.value = 777.0;
    ds_set_finance_if(0, expect, &r);
    finance_rec_t g = ds_get_finance(0);
    if (cases[i].published) {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(777.0f, (float)g.value, cases[i].name);
      TEST_ASSERT_EQUAL_INT_MESSAGE(ST_LIVE, g.hdr.state, cases[i].name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(seeded.id, g.id, cases[i].name);   // seeded id preserved
    } else {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, (float)g.value, cases[i].name);   // dropped: value untouched
      TEST_ASSERT_EQUAL_INT_MESSAGE(ST_LOADING, g.hdr.state, cases[i].name);  // state untouched
    }
  }
}

static void test_reseed_finance_sets_ids_loading_count(void) {
  char ids[3][FIN_ID_LEN] = {"aaa", "bbb", "ccc"};
  ds_reseed_finance(ids, 3);
  TEST_ASSERT_EQUAL_UINT8(3, ds_get_finance_count());
  for (int i = 0; i < 3; i++) {
    finance_rec_t g = ds_get_finance((uint8_t)i);
    TEST_ASSERT_EQUAL_STRING(ids[i], g.id);
    TEST_ASSERT_EQUAL_INT(ST_LOADING, g.hdr.state);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, (float)g.value);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_seeds_finance);
  RUN_TEST(test_set_finance_if_publish_vs_drop);
  RUN_TEST(test_reseed_finance_sets_ids_loading_count);
  RUN_TEST(test_weather_set_get_forces_live);
  RUN_TEST(test_finance_isolation_and_id_preserved);
  RUN_TEST(test_staleness_inclusive_boundary);
  RUN_TEST(test_sweep_never_clobbers_explicit_states);
  RUN_TEST(test_hub_offline_flip_and_recovery);
  RUN_TEST(test_explicit_failure_preserves_payload);
  RUN_TEST(test_tick_buddy_prompt_transitions);
  RUN_TEST(test_tick_buddy_prompt_preserves_hub_offline);
  return UNITY_END();
}
