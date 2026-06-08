#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "core/wifi_list.h"

void setUp(void) {}
void tearDown(void) {}

static void test_append_and_find(void) {
  wifi_list_t l; memset(&l, 0, sizeof(l));
  TEST_ASSERT_TRUE(wifi_list_add(&l, "Home", "pw1"));
  TEST_ASSERT_TRUE(wifi_list_add(&l, "Office", "pw2"));
  TEST_ASSERT_EQUAL(2, l.count);
  TEST_ASSERT_EQUAL(0, wifi_list_find(&l, "Home"));
  TEST_ASSERT_EQUAL(1, wifi_list_find(&l, "Office"));
  TEST_ASSERT_EQUAL(-1, wifi_list_find(&l, "Nope"));
}

static void test_dedup_updates_password(void) {
  wifi_list_t l; memset(&l, 0, sizeof(l));
  wifi_list_add(&l, "Home", "old");
  TEST_ASSERT_TRUE(wifi_list_add(&l, "Home", "new"));   // same SSID => update, not append
  TEST_ASSERT_EQUAL(1, l.count);
  TEST_ASSERT_EQUAL_STRING("new", l.e[0].pass);
}

static void test_reject_when_full_but_update_ok(void) {
  wifi_list_t l; memset(&l, 0, sizeof(l));
  char ssid[16];
  for (int i = 0; i < WIFI_MAX_SAVED; i++) { snprintf(ssid, sizeof(ssid), "net%d", i); TEST_ASSERT_TRUE(wifi_list_add(&l, ssid, "p")); }
  TEST_ASSERT_EQUAL(WIFI_MAX_SAVED, l.count);
  TEST_ASSERT_FALSE(wifi_list_add(&l, "overflow", "p"));   // full + new => reject
  TEST_ASSERT_TRUE(wifi_list_add(&l, "net0", "p2"));       // full but existing => update ok
  TEST_ASSERT_EQUAL(WIFI_MAX_SAVED, l.count);
}

static void test_remove_preserves_order(void) {
  wifi_list_t l; memset(&l, 0, sizeof(l));
  wifi_list_add(&l, "A", "1"); wifi_list_add(&l, "B", "2"); wifi_list_add(&l, "C", "3");
  TEST_ASSERT_TRUE(wifi_list_remove(&l, "B"));
  TEST_ASSERT_EQUAL(2, l.count);
  TEST_ASSERT_EQUAL_STRING("A", l.e[0].ssid);
  TEST_ASSERT_EQUAL_STRING("C", l.e[1].ssid);
  TEST_ASSERT_FALSE(wifi_list_remove(&l, "Z"));            // absent
}

static void test_empty_and_invalid(void) {
  wifi_list_t l; memset(&l, 0, sizeof(l));
  TEST_ASSERT_FALSE(wifi_list_add(&l, "", "p"));           // empty ssid
  TEST_ASSERT_FALSE(wifi_list_add(&l, NULL, "p"));
  TEST_ASSERT_EQUAL(-1, wifi_list_find(&l, "anything"));
  TEST_ASSERT_TRUE(wifi_list_add(&l, "X", NULL));          // null pass ok (open network)
  TEST_ASSERT_EQUAL_STRING("", l.e[0].pass);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_and_find);
  RUN_TEST(test_dedup_updates_password);
  RUN_TEST(test_reject_when_full_but_update_ok);
  RUN_TEST(test_remove_preserves_order);
  RUN_TEST(test_empty_and_invalid);
  return UNITY_END();
}
