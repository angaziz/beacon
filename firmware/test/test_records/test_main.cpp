#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "core/records.h"

void setUp(void) {}
void tearDown(void) {}

// record_age_s, table-driven (incl. the never-updated sentinel).
static void test_record_age(void) {
  struct { uint32_t last_updated; uint32_t now; uint32_t expect; } cases[] = {
    {0,    1000, UINT32_MAX},  // never updated
    {1000, 1000, 0},           // just updated
    {1000, 1300, 300},         // 5 min old
    {500,  1000, 500},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    record_hdr_t h = { cases[i].last_updated, ST_LIVE, ERR_NONE };
    TEST_ASSERT_EQUAL_UINT32(cases[i].expect, record_age_s(&h, cases[i].now));
  }
}

// The four domain records compile + embed the header; frozen constants + usage null sentinel hold.
static void test_records_contract(void) {
  weather_rec_t w; finance_rec_t f; usage_rec_t u; buddy_rec_t b;
  (void)w; (void)u; (void)b;

  TEST_ASSERT_EQUAL_INT(16,  FIN_ID_LEN);
  TEST_ASSERT_EQUAL_INT(80,  BUDDY_HINT_LEN);
  TEST_ASSERT_EQUAL_INT(4,   USAGE_PROVIDERS_MAX);
  TEST_ASSERT_EQUAL_INT(13,  USAGE_ID_LEN);
  TEST_ASSERT_EQUAL_INT(11,  USAGE_LABEL_LEN);

  usage_window_t win = { -1, 0 };          // -1 = unavailable
  TEST_ASSERT_EQUAL_INT16(-1, win.pct);

  u.count = 2;                             // provider array + per-provider id/label reachable
  strcpy(u.p[0].id, "claude"); strcpy(u.p[0].label, "CLAUDE");
  TEST_ASSERT_EQUAL_UINT8(2, u.count);
  TEST_ASSERT_EQUAL_STRING("claude", u.p[0].id);
  strcpy(b.prompt.agent, "codex");         // additive agent fields reachable
  strcpy(b.sessions[0].agent, "claude");
  TEST_ASSERT_EQUAL_STRING("codex", b.prompt.agent);
  TEST_ASSERT_EQUAL_STRING("claude", b.sessions[0].agent);

  f.hdr.state = ST_STALE;                  // header reachable on every record
  TEST_ASSERT_EQUAL_INT(ST_STALE, f.hdr.state);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_age);
  RUN_TEST(test_records_contract);
  return UNITY_END();
}
