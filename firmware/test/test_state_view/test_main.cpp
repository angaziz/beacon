#include <unity.h>
#include <string.h>
#include "ui/state_view.h"

void setUp(void) {} void tearDown(void) {}

static void test_age_str(void) {
  char b[8];
  age_str(b, sizeof(b), UINT32_MAX); TEST_ASSERT_EQUAL_STRING("--", b);
  age_str(b, sizeof(b), 0);   TEST_ASSERT_EQUAL_STRING("now", b);
  age_str(b, sizeof(b), 42);  TEST_ASSERT_EQUAL_STRING("42s", b);
  age_str(b, sizeof(b), 120); TEST_ASSERT_EQUAL_STRING("2m", b);
  age_str(b, sizeof(b), 7200);TEST_ASSERT_EQUAL_STRING("2h", b);
  age_str(b, sizeof(b), 172800);TEST_ASSERT_EQUAL_STRING("2d", b);
}
static void test_sv_status(void) {
  char b[16]; record_hdr_t h; h.last_updated = 1000; h.err = ERR_NONE;
  h.state = ST_LIVE;        TEST_ASSERT_FALSE(sv_status(b, sizeof(b), &h, 2000));
  h.state = ST_STALE;       TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 1000 + 120)); TEST_ASSERT_EQUAL_STRING("STALE 2m", b);
  h.state = ST_OFFLINE;     TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 2000)); TEST_ASSERT_EQUAL_STRING("OFFLINE", b);
  h.state = ST_HUB_OFFLINE; TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 1000 + 1000)); TEST_ASSERT_EQUAL_STRING("HUB OFFLINE 16m", b);
  h.state = ST_ERROR; h.err = ERR_RATE_LIMITED; TEST_ASSERT_TRUE(sv_status(b, sizeof(b), &h, 2000)); TEST_ASSERT_EQUAL_STRING("RATE LIMIT", b);
}
static void test_predicates(void) {
  TEST_ASSERT_TRUE(sv_dim(ST_STALE));   TEST_ASSERT_FALSE(sv_dim(ST_LIVE));
  TEST_ASSERT_TRUE(sv_placeholder(ST_LOADING)); TEST_ASSERT_FALSE(sv_placeholder(ST_STALE));
  TEST_ASSERT_TRUE(sv_severe(ST_OFFLINE)); TEST_ASSERT_FALSE(sv_severe(ST_STALE));
}
int main(int, char**) { UNITY_BEGIN(); RUN_TEST(test_age_str); RUN_TEST(test_sv_status); RUN_TEST(test_predicates); return UNITY_END(); }
