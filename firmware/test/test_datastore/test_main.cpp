#include <unity.h>
#include <string.h>
#include "core/datastore.h"
#include "config/tickers.h"

void setUp(void) { datastore_init(); }   // reset the static store before each test
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_seeds_finance);
  RUN_TEST(test_weather_set_get_forces_live);
  RUN_TEST(test_finance_isolation_and_id_preserved);
  RUN_TEST(test_staleness_inclusive_boundary);
  RUN_TEST(test_sweep_never_clobbers_explicit_states);
  RUN_TEST(test_hub_offline_flip_and_recovery);
  RUN_TEST(test_explicit_failure_preserves_payload);
  return UNITY_END();
}
