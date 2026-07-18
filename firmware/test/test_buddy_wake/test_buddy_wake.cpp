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

// Mirror of the session-id diff added to buddy_wake_service: wakes on a brand-new session id
// while the PAL mascot overlay is left open across sleep (gating on idle_is_inactive() +
// pal_panel_is_open() is a trivial AND at the call site, not worth mirroring here).
#define SID_LEN 8
#define SESSIONS_MAX 5

typedef struct {
  bool primed;
  char prev_ids[SESSIONS_MAX][SID_LEN];
  int  prev_count;
} session_wake_state_t;

static bool session_wake_check(session_wake_state_t* s, const char ids[][SID_LEN], int count) {
  bool new_session = false;
  if (s->primed) {
    for (int i = 0; i < count && !new_session; i++) {
      bool seen = false;
      for (int j = 0; j < s->prev_count && !seen; j++)
        seen = strncmp(s->prev_ids[j], ids[i], SID_LEN) == 0;
      new_session = !seen;
    }
  }
  s->prev_count = count;
  for (int i = 0; i < count; i++) strncpy(s->prev_ids[i], ids[i], SID_LEN);
  s->primed = true;
  return new_session;
}

// The very first call only primes the snapshot -- it must never itself report "new" (that
// would wake the device on every boot/reconnect, not just on a session that starts afterward).
static void test_no_wake_on_first_frame(void) {
  session_wake_state_t s = {};
  const char ids[][SID_LEN] = {"s1"};
  TEST_ASSERT_FALSE(session_wake_check(&s, ids, 1));
}

// A session id absent from the primed snapshot is a new session.
static void test_new_session_after_priming(void) {
  session_wake_state_t s = {};
  const char first[][SID_LEN] = {"s1"};
  session_wake_check(&s, first, 1);
  const char second[][SID_LEN] = {"s1", "s2"};
  TEST_ASSERT_TRUE(session_wake_check(&s, second, 2));
}

// Same id set on the next poll is not a new session.
static void test_no_new_session_same_ids(void) {
  session_wake_state_t s = {};
  const char ids[][SID_LEN] = {"s1"};
  session_wake_check(&s, ids, 1);
  TEST_ASSERT_FALSE(session_wake_check(&s, ids, 1));
}

// s1 finishing while s2 starts still counts as new (s2 wasn't in the previous snapshot).
static void test_session_replaced_counts_as_new(void) {
  session_wake_state_t s = {};
  const char first[][SID_LEN] = {"s1"};
  session_wake_check(&s, first, 1);
  const char second[][SID_LEN] = {"s2"};
  TEST_ASSERT_TRUE(session_wake_check(&s, second, 1));
}

// No sessions before or after: nothing to wake for.
static void test_empty_to_empty_no_wake(void) {
  session_wake_state_t s = {};
  TEST_ASSERT_FALSE(session_wake_check(&s, NULL, 0));
  TEST_ASSERT_FALSE(session_wake_check(&s, NULL, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_edge_when_idle);
  RUN_TEST(test_rising_edge_on_first_need);
  RUN_TEST(test_no_repeated_edge_same_prompt);
  RUN_TEST(test_edge_after_clear);
  RUN_TEST(test_new_prompt_id_while_needing);
  RUN_TEST(test_session_needs_rising_edge);
  RUN_TEST(test_no_wake_on_first_frame);
  RUN_TEST(test_new_session_after_priming);
  RUN_TEST(test_no_new_session_same_ids);
  RUN_TEST(test_session_replaced_counts_as_new);
  RUN_TEST(test_empty_to_empty_no_wake);
  return UNITY_END();
}
