#include <unity.h>
#include <string.h>
#include "core/about_format.h"

void setUp(void) {} void tearDown(void) {}

static void test_mac(void) {
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x0D, 0xE0, 0xFF};
  char b[18];
  about_fmt_mac(mac, b);
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:0D:E0:FF", b);
}

static void test_uptime(void) {
  struct { uint32_t secs; const char* want; } cases[] = {
    {0,      "0m"},
    {30,     "0m"},     // floor to 0m under a minute
    {59,     "0m"},
    {60,     "1m"},
    {200,    "3m"},
    {3599,   "59m"},
    {3600,   "1h 0m"},
    {11520,  "3h 12m"}, // 3*3600 + 12*60
    {86399,  "23h 59m"},
    {86400,  "1d 0h"},
    {187200, "2d 4h"},  // 2*86400 + 4*3600
  };
  char b[16];
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    about_fmt_uptime(cases[i].secs, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(cases[i].want, b);
  }
}

static void test_heap_kb(void) {
  struct { uint32_t bytes; const char* want; } cases[] = {
    {0,      "0 KB"},
    {1023,   "0 KB"},   // truncated
    {1024,   "1 KB"},
    {145408, "142 KB"},
  };
  char b[16];
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    about_fmt_heap_kb(cases[i].bytes, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(cases[i].want, b);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mac);
  RUN_TEST(test_uptime);
  RUN_TEST(test_heap_kb);
  return UNITY_END();
}
