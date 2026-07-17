#include <unity.h>
#include "ui/screens/pal_state.h"

static buddy_rec_t zeroed(void) {
  buddy_rec_t b = {};
  b.hdr.state = ST_LIVE;
  return b;
}

void test_hub_offline_is_sleep(void) {
  buddy_rec_t b = zeroed();
  b.hdr.state = ST_HUB_OFFLINE;
  b.session_count = 1;
  b.sessions[0].state = BST_WORKING;
  TEST_ASSERT_EQUAL(PAL_STATE_SLEEP, pal_pick_state(&b));
}

void test_no_sessions_is_sleep(void) {
  buddy_rec_t b = zeroed();
  b.session_count = 0;
  TEST_ASSERT_EQUAL(PAL_STATE_SLEEP, pal_pick_state(&b));
}

void test_prompt_wins_over_working_session(void) {
  buddy_rec_t b = zeroed();
  b.session_count = 1;
  b.sessions[0].state = BST_WORKING;
  b.prompt.present = true;
  TEST_ASSERT_EQUAL(PAL_STATE_NOTIFY, pal_pick_state(&b));
}

void test_working_session_without_prompt_is_active(void) {
  buddy_rec_t b = zeroed();
  b.session_count = 2;
  b.sessions[0].state = BST_IDLE;
  b.sessions[1].state = BST_WORKING;
  TEST_ASSERT_EQUAL(PAL_STATE_ACTIVE, pal_pick_state(&b));
}

void test_idle_sessions_no_prompt_is_idle(void) {
  buddy_rec_t b = zeroed();
  b.session_count = 2;
  b.sessions[0].state = BST_IDLE;
  b.sessions[1].state = BST_WAITING;
  TEST_ASSERT_EQUAL(PAL_STATE_IDLE, pal_pick_state(&b));
}

void test_anim_mapping(void) {
  TEST_ASSERT_EQUAL(PAL_ANIM_EXPRESSION_SLEEP, pal_anim_for_state(PAL_STATE_SLEEP, 0));
  TEST_ASSERT_EQUAL(PAL_ANIM_DANCE_BOUNCE, pal_anim_for_state(PAL_STATE_NOTIFY, 0));
  TEST_ASSERT_EQUAL(PAL_ANIM_WORK_CODING, pal_anim_for_state(PAL_STATE_ACTIVE, 0));
  TEST_ASSERT_EQUAL(PAL_IDLE_POOL[0], pal_anim_for_state(PAL_STATE_IDLE, 0));
  TEST_ASSERT_EQUAL(PAL_IDLE_POOL[0], pal_anim_for_state(PAL_STATE_IDLE, PAL_IDLE_POOL_LEN));
}

void setUp(void) {}
void tearDown(void) {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_hub_offline_is_sleep);
  RUN_TEST(test_no_sessions_is_sleep);
  RUN_TEST(test_prompt_wins_over_working_session);
  RUN_TEST(test_working_session_without_prompt_is_active);
  RUN_TEST(test_idle_sessions_no_prompt_is_idle);
  RUN_TEST(test_anim_mapping);
  return UNITY_END();
}
