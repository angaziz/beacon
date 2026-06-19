#include <unity.h>
#include "ui/screens/screen_common.h"
#include <string.h>

void test_badge_hidden_when_single(void) {
    char out[24]; buddy_queue_badge(1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}
void test_badge_shows_one_of_n(void) {
    char out[24]; buddy_queue_badge(3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(" (1 of 3)", out);
}
void setUp(void){} void tearDown(void){}
int main(){ UNITY_BEGIN(); RUN_TEST(test_badge_hidden_when_single); RUN_TEST(test_badge_shows_one_of_n); return UNITY_END(); }
