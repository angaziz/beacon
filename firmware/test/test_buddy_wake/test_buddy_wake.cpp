// Tests the rising-edge + new-prompt detection logic that drives buddy_wake_service.
// The production function depends on LVGL and carousel (device-only), so we replicate
// the same state machine here in a pure function to make it host-testable.
#include <unity.h>
#include <stdbool.h>
#include <string.h>

void setUp(void) {} void tearDown(void) {}

// Mirror the detection state used by buddy_wake_service.
typedef struct {
  bool prev_needs;
  char prev_prompt[32];
} wake_state_t;

// Pure rising-edge check extracted from buddy_wake_service.
// Returns true on a rising edge (false->true) or a new prompt-id while needing.
static bool wake_check(wake_state_t* s, bool needs_user, const char* prompt_id) {
  bool rising = (!s->prev_needs && needs_user) ||
                (needs_user && prompt_id && prompt_id[0] != '\0' &&
                 strncmp(s->prev_prompt, prompt_id, 31) != 0);
  s->prev_needs = needs_user;
  if (prompt_id && needs_user) {
    strncpy(s->prev_prompt, prompt_id, 31);
    s->prev_prompt[31] = '\0';
  } else {
    s->prev_prompt[0] = '\0';
  }
  return rising;
}

// --- Tests ---

// No needs => no rising edge on any call.
static void test_no_edge_when_idle(void) {
  wake_state_t s = {false, ""};
  TEST_ASSERT_FALSE(wake_check(&s, false, ""));
  TEST_ASSERT_FALSE(wake_check(&s, false, ""));
}

// false -> true is a rising edge.
static void test_rising_edge_on_first_need(void) {
  wake_state_t s = {false, ""};
  TEST_ASSERT_TRUE(wake_check(&s, true, "p1"));
}

// true -> true (same prompt) is NOT a repeated edge.
static void test_no_repeated_edge_same_prompt(void) {
  wake_state_t s = {false, ""};
  wake_check(&s, true, "p1");               // consume the rising edge
  TEST_ASSERT_FALSE(wake_check(&s, true, "p1"));
  TEST_ASSERT_FALSE(wake_check(&s, true, "p1"));
}

// true -> false -> true is a new rising edge.
static void test_edge_after_clear(void) {
  wake_state_t s = {false, ""};
  wake_check(&s, true, "p1");
  wake_check(&s, false, "");
  TEST_ASSERT_TRUE(wake_check(&s, true, "p2"));
}

// New prompt id while already needing is also a rising edge (queued prompt arrived).
static void test_new_prompt_id_while_needing(void) {
  wake_state_t s = {false, ""};
  wake_check(&s, true, "p1");               // first prompt: rising edge (consumed)
  TEST_ASSERT_TRUE(wake_check(&s, true, "p2"));   // second prompt: re-trigger
  TEST_ASSERT_FALSE(wake_check(&s, true, "p2"));  // same prompt again: no edge
}

// Session-only needs (no prompt id) follow the same false->true rule.
static void test_session_needs_rising_edge(void) {
  wake_state_t s = {false, ""};
  TEST_ASSERT_TRUE(wake_check(&s, true, ""));
  TEST_ASSERT_FALSE(wake_check(&s, true, ""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_edge_when_idle);
  RUN_TEST(test_rising_edge_on_first_need);
  RUN_TEST(test_no_repeated_edge_same_prompt);
  RUN_TEST(test_edge_after_clear);
  RUN_TEST(test_new_prompt_id_while_needing);
  RUN_TEST(test_session_needs_rising_edge);
  return UNITY_END();
}
