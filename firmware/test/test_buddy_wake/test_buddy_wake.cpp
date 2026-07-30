// Tests buddy_wake_eval (core/buddy_wake.cpp) -- the notable-event detector behind auto-wake.
// The production function is pure, so these exercise it directly (no mirrored copy to drift).
#include <unity.h>
#include <string.h>
#include "core/buddy_wake.h"

void setUp(void) {} void tearDown(void) {}

// --- snapshot builder --------------------------------------------------------

typedef struct { const char* id; uint8_t state; } sess_spec_t;

// `ts` churns on every real hub frame, so every snapshot here carries a different one: any case
// that expects WAKE_ACT_NONE is also proving ts is not a wake signal.
static buddy_rec_t mk(const sess_spec_t* s, uint8_t n, const char* prompt_id, uint32_t ts) {
  buddy_rec_t b;
  memset(&b, 0, sizeof(b));
  for (uint8_t i = 0; i < n; i++) {
    strncpy(b.sessions[i].id, s[i].id, BUDDY_SID_LEN - 1);
    b.sessions[i].state = s[i].state;
    b.sessions[i].ts    = ts + i;
  }
  b.session_count = n;
  if (prompt_id && prompt_id[0]) {
    b.prompt.present = true;
    strncpy(b.prompt.id, prompt_id, BUDDY_ID_LEN - 1);
  }
  return b;
}

static const sess_spec_t S_WORK1[]      = {{"s1", BST_WORKING}};
static const sess_spec_t S_WORK12[]     = {{"s1", BST_WORKING}, {"s2", BST_WORKING}};
static const sess_spec_t S_WORK21[]     = {{"s2", BST_WORKING}, {"s1", BST_WORKING}};
static const sess_spec_t S_WAIT1[]      = {{"s1", BST_WAITING}};
static const sess_spec_t S_ATTN1[]      = {{"s1", BST_ATTENTION}};
static const sess_spec_t S_IDLE1[]      = {{"s1", BST_IDLE}};
static const sess_spec_t S_QUES1[]      = {{"s1", BST_QUESTION}};

// --- table-driven: prime one snapshot, assert the action on the next --------

typedef struct {
  const char*         name;
  const sess_spec_t*  before; uint8_t before_n; const char* before_prompt;
  const sess_spec_t*  after;  uint8_t after_n;  const char* after_prompt;
  uint8_t             mode;
  buddy_wake_action_t expect;
} case_t;

static const case_t CASES[] = {
  // routine churn must never wake
  {"ts bump only",        S_WORK12, 2, "",   S_WORK12, 2, "",   WAKE_EVERYTHING, WAKE_ACT_NONE},
  {"list reordered",      S_WORK12, 2, "",   S_WORK21, 2, "",   WAKE_EVERYTHING, WAKE_ACT_NONE},
  {"session vanished",    S_WORK12, 2, "",   S_WORK1,  1, "",   WAKE_EVERYTHING, WAKE_ACT_NONE},
  {"same prompt id",      NULL,     0, "p1", NULL,     0, "p1", WAKE_ATTENTION,  WAKE_ACT_NONE},

  // session churn: wake, but stay on the current screen
  {"first ever session",  NULL,     0, "",   S_WORK1,  1, "",   WAKE_EVERYTHING, WAKE_ACT_ONLY},
  {"new session id",      S_WORK1,  1, "",   S_WORK12, 2, "",   WAKE_EVERYTHING, WAKE_ACT_ONLY},
  {"working -> idle",     S_WORK1,  1, "",   S_IDLE1,  1, "",   WAKE_EVERYTHING, WAKE_ACT_ONLY},
  {"working -> question", S_WORK1,  1, "",   S_QUES1,  1, "",   WAKE_EVERYTHING, WAKE_ACT_ONLY},

  // needs-user: wake AND jump to the buddy screen
  {"working -> waiting",  S_WORK1,  1, "",   S_WAIT1,  1, "",   WAKE_EVERYTHING, WAKE_ACT_SHOW},
  {"working -> attention",S_WORK1,  1, "",   S_ATTN1,  1, "",   WAKE_ATTENTION,  WAKE_ACT_SHOW},
  {"prompt arrives",      NULL,     0, "",   NULL,     0, "p1", WAKE_PROMPTS,    WAKE_ACT_SHOW},
  {"new prompt id",       NULL,     0, "p1", NULL,     0, "p2", WAKE_ATTENTION,  WAKE_ACT_SHOW},

  // the mode ladder gates which events count
  {"waiting @ prompts",   S_WORK1,  1, "",   S_WAIT1,  1, "",   WAKE_PROMPTS,    WAKE_ACT_NONE},
  {"new session @ attn",  S_WORK1,  1, "",   S_WORK12, 2, "",   WAKE_ATTENTION,  WAKE_ACT_NONE},
  {"-> idle @ attention", S_WORK1,  1, "",   S_IDLE1,  1, "",   WAKE_ATTENTION,  WAKE_ACT_NONE},
  {"prompt @ off",        NULL,     0, "",   NULL,     0, "p1", WAKE_OFF,        WAKE_ACT_NONE},
  {"new session @ off",   S_WORK1,  1, "",   S_WORK12, 2, "",   WAKE_OFF,        WAKE_ACT_NONE},
};

static void test_table(void) {
  for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
    const case_t* c = &CASES[i];
    buddy_wake_state_t st;
    memset(&st, 0, sizeof(st));
    buddy_rec_t before = mk(c->before, c->before_n, c->before_prompt, 1000);
    buddy_rec_t after  = mk(c->after,  c->after_n,  c->after_prompt,  2000);
    buddy_wake_eval(&st, &before, c->mode, 100, false);   // prime the baseline while awake
    buddy_wake_action_t got = buddy_wake_eval(&st, &after, c->mode, 200, true);
    TEST_ASSERT_EQUAL_MESSAGE(c->expect, got, c->name);
  }
}

// --- gating, cooldown, and baseline priming ---------------------------------

// An event that lands while the screen is on fires nothing AND consumes the edge: replaying the
// same snapshot after the device falls asleep must stay quiet (no stale wake).
static void test_awake_suppresses_and_rebases(void) {
  buddy_wake_state_t st; memset(&st, 0, sizeof(st));
  buddy_rec_t work = mk(S_WORK1, 1, "", 1000);
  buddy_rec_t wait = mk(S_WAIT1, 1, "", 2000);
  buddy_wake_eval(&st, &work, WAKE_EVERYTHING, 100, false);
  TEST_ASSERT_EQUAL(WAKE_ACT_NONE, buddy_wake_eval(&st, &wait, WAKE_EVERYTHING, 200, false));
  TEST_ASSERT_EQUAL(WAKE_ACT_NONE, buddy_wake_eval(&st, &wait, WAKE_EVERYTHING, 300, true));
}

// A second session event inside BUDDY_WAKE_COOLDOWN_S is swallowed; one after it is not.
static void test_cooldown_gates_wake_only(void) {
  buddy_wake_state_t st; memset(&st, 0, sizeof(st));
  buddy_rec_t work = mk(S_WORK1, 1, "", 1000);
  buddy_rec_t idle = mk(S_IDLE1, 1, "", 2000);
  buddy_rec_t ques = mk(S_QUES1, 1, "", 3000);
  buddy_wake_eval(&st, &work, WAKE_EVERYTHING, 100, false);
  TEST_ASSERT_EQUAL(WAKE_ACT_ONLY, buddy_wake_eval(&st, &idle, WAKE_EVERYTHING, 200, true));
  TEST_ASSERT_EQUAL(WAKE_ACT_NONE, buddy_wake_eval(&st, &ques, WAKE_EVERYTHING,
                                                   200 + BUDDY_WAKE_COOLDOWN_S - 1, true));
}

static void test_cooldown_expires(void) {
  buddy_wake_state_t st; memset(&st, 0, sizeof(st));
  buddy_rec_t work = mk(S_WORK1, 1, "", 1000);
  buddy_rec_t idle = mk(S_IDLE1, 1, "", 2000);
  buddy_rec_t ques = mk(S_QUES1, 1, "", 3000);
  buddy_wake_eval(&st, &work, WAKE_EVERYTHING, 100, false);
  TEST_ASSERT_EQUAL(WAKE_ACT_ONLY, buddy_wake_eval(&st, &idle, WAKE_EVERYTHING, 200, true));
  TEST_ASSERT_EQUAL(WAKE_ACT_ONLY, buddy_wake_eval(&st, &ques, WAKE_EVERYTHING,
                                                   200 + BUDDY_WAKE_COOLDOWN_S, true));
}

// A permission prompt must get through even one second after a wake-only fired.
static void test_cooldown_never_gates_needs_user(void) {
  buddy_wake_state_t st; memset(&st, 0, sizeof(st));
  buddy_rec_t work   = mk(S_WORK1, 1, "",   1000);
  buddy_rec_t idle   = mk(S_IDLE1, 1, "",   2000);
  buddy_rec_t prompt = mk(S_IDLE1, 1, "p1", 3000);
  buddy_wake_eval(&st, &work, WAKE_EVERYTHING, 100, false);
  TEST_ASSERT_EQUAL(WAKE_ACT_ONLY, buddy_wake_eval(&st, &idle,   WAKE_EVERYTHING, 200, true));
  TEST_ASSERT_EQUAL(WAKE_ACT_SHOW, buddy_wake_eval(&st, &prompt, WAKE_EVERYTHING, 201, true));
}

// needs-user falling and rising again is a fresh edge (the classic prompt -> resolved -> prompt).
static void test_edge_after_clear(void) {
  buddy_wake_state_t st; memset(&st, 0, sizeof(st));
  buddy_rec_t p1   = mk(NULL, 0, "p1", 1000);
  buddy_rec_t none = mk(NULL, 0, "",   2000);
  buddy_rec_t p2   = mk(NULL, 0, "p2", 3000);
  TEST_ASSERT_EQUAL(WAKE_ACT_SHOW, buddy_wake_eval(&st, &p1,   WAKE_PROMPTS, 100, true));
  TEST_ASSERT_EQUAL(WAKE_ACT_NONE, buddy_wake_eval(&st, &none, WAKE_PROMPTS, 110, true));
  TEST_ASSERT_EQUAL(WAKE_ACT_SHOW, buddy_wake_eval(&st, &p2,   WAKE_PROMPTS, 120, true));
}

// More sessions than the record holds must not run off the end of the baseline arrays.
static void test_session_count_is_clamped(void) {
  buddy_wake_state_t st; memset(&st, 0, sizeof(st));
  buddy_rec_t b = mk(S_WORK1, 1, "", 1000);
  b.session_count = BUDDY_SESSIONS_MAX + 3;                 // liar count; sessions[] is untouched
  TEST_ASSERT_EQUAL(WAKE_ACT_ONLY, buddy_wake_eval(&st, &b, WAKE_EVERYTHING, 100, true));
  TEST_ASSERT_EQUAL(BUDDY_SESSIONS_MAX, st.prev_count);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_table);
  RUN_TEST(test_awake_suppresses_and_rebases);
  RUN_TEST(test_cooldown_gates_wake_only);
  RUN_TEST(test_cooldown_expires);
  RUN_TEST(test_cooldown_never_gates_needs_user);
  RUN_TEST(test_edge_after_clear);
  RUN_TEST(test_session_count_is_clamped);
  return UNITY_END();
}
