#include <unity.h>
#include <string.h>
#include "core/hub_proto.h"

void setUp(void) {}
void tearDown(void) {}

// --- reassembler capture harness ---
#define CAP_MAX 8
static char   g_frames[CAP_MAX][HUB_FRAME_MAX];
static size_t g_count;
static void cap_cb(const char* line, size_t len, void* user) {
  (void)user;
  if (g_count < CAP_MAX) { memcpy(g_frames[g_count], line, len); g_frames[g_count][len] = '\0'; g_count++; }
}
static void cap_reset(void) { g_count = 0; }

// Canonical tech.md §7.1 status frame (the device-facing schema is frozen).
static const char* FRAME_FULL =
  "{\"v\":1,\"usage\":{\"claude\":{\"h5\":{\"pct\":24,\"reset\":1717600000},"
  "\"d7\":{\"pct\":24,\"reset\":1717800000}},"
  "\"codex\":{\"h5\":{\"pct\":1,\"reset\":1717590000},\"d7\":{\"pct\":29,\"reset\":1717800000}}},"
  "\"buddy\":{\"running\":2,\"waiting\":1,\"tokens\":184502,\"context_pct\":42,"
  "\"entries\":[\"10:42 git push\",\"10:41 yarn test\"],"
  "\"prompt\":{\"id\":\"req_abc\",\"tool\":\"Bash\",\"hint\":\"rm -rf /tmp/build\"}}}";

// ===== reassembly =====

static void test_reassemble_single_frame(void) {
  cap_reset();
  hub_reassembler_t r; hub_reassembler_reset(&r);
  const char* in = "{\"v\":1}\n";
  hub_reassembler_feed(&r, in, strlen(in), cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(1, g_count);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1}", g_frames[0]);
}

static void test_reassemble_two_frames_one_feed(void) {
  cap_reset();
  hub_reassembler_t r; hub_reassembler_reset(&r);
  const char* in = "{\"a\":1}\n{\"b\":2}\n";
  hub_reassembler_feed(&r, in, strlen(in), cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(2, g_count);
  TEST_ASSERT_EQUAL_STRING("{\"a\":1}", g_frames[0]);
  TEST_ASSERT_EQUAL_STRING("{\"b\":2}", g_frames[1]);
}

static void test_reassemble_split_across_feeds(void) {
  cap_reset();
  hub_reassembler_t r; hub_reassembler_reset(&r);
  hub_reassembler_feed(&r, "{\"hel", 5, cap_cb, NULL);   // frame split mid-way over BLE writes
  hub_reassembler_feed(&r, "lo\":1}", 6, cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(0, g_count);                  // no newline yet
  hub_reassembler_feed(&r, "\n", 1, cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(1, g_count);
  TEST_ASSERT_EQUAL_STRING("{\"hello\":1}", g_frames[0]);
}

static void test_reassemble_crlf_and_empty(void) {
  cap_reset();
  hub_reassembler_t r; hub_reassembler_reset(&r);
  const char* in = "{\"x\":1}\r\n\n{\"y\":2}\n";   // CRLF stripped; the empty line skipped
  hub_reassembler_feed(&r, in, strlen(in), cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(2, g_count);
  TEST_ASSERT_EQUAL_STRING("{\"x\":1}", g_frames[0]);
  TEST_ASSERT_EQUAL_STRING("{\"y\":2}", g_frames[1]);
}

static void test_reassemble_overflow_then_recover(void) {
  cap_reset();
  hub_reassembler_t r; hub_reassembler_reset(&r);
  char big[HUB_FRAME_MAX + 200];
  memset(big, 'x', sizeof(big)); big[sizeof(big) - 1] = '\n';   // one oversize frame
  hub_reassembler_feed(&r, big, sizeof(big), cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(0, g_count);     // dropped
  TEST_ASSERT_EQUAL_UINT32(1, r.drops);
  const char* ok = "{\"ok\":1}\n";           // next frame parses fine
  hub_reassembler_feed(&r, ok, strlen(ok), cap_cb, NULL);
  TEST_ASSERT_EQUAL_size_t(1, g_count);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":1}", g_frames[0]);
}

// ===== status parse =====

static void test_parse_full_frame(void) {
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  TEST_ASSERT_TRUE(hub_parse_status(FRAME_FULL, strlen(FRAME_FULL), &u, &hu, &b, &hb));
  TEST_ASSERT_TRUE(hu); TEST_ASSERT_TRUE(hb);
  TEST_ASSERT_EQUAL_INT16(24, u.claude.h5.pct);
  TEST_ASSERT_EQUAL_UINT32(1717600000, u.claude.h5.reset);
  TEST_ASSERT_EQUAL_INT16(29, u.codex.d7.pct);
  TEST_ASSERT_EQUAL_UINT8(2, b.running);
  TEST_ASSERT_EQUAL_UINT8(1, b.waiting);
  TEST_ASSERT_EQUAL_UINT32(184502, b.tokens);
  TEST_ASSERT_EQUAL_UINT8(42, b.context_pct);
  TEST_ASSERT_EQUAL_UINT8(2, b.entry_count);
  TEST_ASSERT_EQUAL_STRING("10:42 git push", b.entries[0]);
  TEST_ASSERT_TRUE(b.prompt.present);
  TEST_ASSERT_EQUAL_STRING("req_abc", b.prompt.id);
  TEST_ASSERT_EQUAL_STRING("Bash", b.prompt.tool);
  TEST_ASSERT_EQUAL_STRING("rm -rf /tmp/build", b.prompt.hint);
}

static void test_parse_null_and_missing_windows(void) {
  // claude.h5.pct explicitly null; codex provider entirely absent => all unavailable (-1).
  const char* j = "{\"v\":1,\"usage\":{\"claude\":{\"h5\":{\"pct\":null,\"reset\":0},"
                  "\"d7\":{\"pct\":50,\"reset\":123}}}}";
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
  TEST_ASSERT_TRUE(hu); TEST_ASSERT_FALSE(hb);
  TEST_ASSERT_EQUAL_INT16(-1, u.claude.h5.pct);   // explicit JSON null
  TEST_ASSERT_EQUAL_INT16(50, u.claude.d7.pct);
  TEST_ASSERT_EQUAL_INT16(-1, u.codex.h5.pct);    // missing provider
  TEST_ASSERT_EQUAL_INT16(-1, u.codex.d7.pct);
}

static void test_parse_prompt_absent_is_idle(void) {
  const char* j = "{\"v\":1,\"buddy\":{\"running\":0,\"waiting\":0,\"tokens\":10,\"context_pct\":5}}";
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
  TEST_ASSERT_TRUE(hb);
  TEST_ASSERT_FALSE(b.prompt.present);            // absence of prompt => idle
}

// A full-frame resend repeating the SAME pending prompt must NOT reset the confirm state (#8),
// or the pending ack would no longer transition the prompt.
static void test_parse_same_prompt_preserves_pending(void) {
  const char* j = "{\"v\":1,\"buddy\":{\"running\":0,\"waiting\":0,\"tokens\":0,\"context_pct\":0,"
                  "\"prompt\":{\"id\":\"p7\",\"tool\":\"Bash\",\"hint\":\"ls\"}}}";
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  b.prompt.present = true; strcpy(b.prompt.id, "p7"); b.prompt.decision_state = PROMPT_PENDING;
  TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
  TEST_ASSERT_EQUAL_UINT8(PROMPT_PENDING, b.prompt.decision_state);
}

// A different prompt id IS fresh to decide => reset to idle.
static void test_parse_new_prompt_resets_decision(void) {
  const char* j = "{\"v\":1,\"buddy\":{\"running\":0,\"waiting\":0,\"tokens\":0,\"context_pct\":0,"
                  "\"prompt\":{\"id\":\"p8\",\"tool\":\"Bash\",\"hint\":\"ls\"}}}";
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  b.prompt.present = true; strcpy(b.prompt.id, "p7"); b.prompt.decision_state = PROMPT_PENDING;
  TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
  TEST_ASSERT_EQUAL_UINT8(PROMPT_IDLE_DECISION, b.prompt.decision_state);
}

static void test_parse_truncates_long_strings(void) {
  // id longer than BUDDY_ID_LEN-1 (23) must truncate, not overflow (hub mints <=23, but be safe).
  const char* j = "{\"v\":1,\"buddy\":{\"prompt\":{\"id\":\"0123456789ABCDEF0123456789\","
                  "\"tool\":\"Bash\",\"hint\":\"x\"}}}";
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
  TEST_ASSERT_TRUE(b.prompt.present);
  TEST_ASSERT_EQUAL_size_t(BUDDY_ID_LEN - 1, strlen(b.prompt.id));   // truncated to capacity-1
  TEST_ASSERT_EQUAL_STRING_LEN("0123456789ABCDEF0123456", b.prompt.id, BUDDY_ID_LEN - 1);
}

static void test_parse_entries_capped(void) {
  const char* j = "{\"v\":1,\"buddy\":{\"entries\":[\"a\",\"b\",\"c\",\"d\",\"e\"]}}";  // > BUDDY_ENTRIES
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  memset(&u, 0, sizeof(u)); memset(&b, 0, sizeof(b));
  TEST_ASSERT_TRUE(hub_parse_status(j, strlen(j), &u, &hu, &b, &hb));
  TEST_ASSERT_EQUAL_UINT8(BUDDY_ENTRIES, b.entry_count);
}

static void test_parse_rejects_bad_version_and_garbage(void) {
  usage_rec_t u; buddy_rec_t b; bool hu, hb;
  const char* v2 = "{\"v\":2,\"usage\":{}}";
  TEST_ASSERT_FALSE(hub_parse_status(v2, strlen(v2), &u, &hu, &b, &hb));
  const char* nov = "{\"usage\":{}}";              // missing v
  TEST_ASSERT_FALSE(hub_parse_status(nov, strlen(nov), &u, &hu, &b, &hb));
  const char* junk = "not json at all";
  TEST_ASSERT_FALSE(hub_parse_status(junk, strlen(junk), &u, &hu, &b, &hb));
}

// ===== command build =====

static void test_build_permission_roundtrips(void) {
  char buf[128];
  size_t n = hub_build_permission(buf, sizeof(buf), "req_abc", true);
  TEST_ASSERT_GREATER_THAN_size_t(0, n);
  TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);        // newline-terminated
  TEST_ASSERT_EQUAL_CHAR('\0', buf[n]);
  // re-parse to assert shape (id echo + decision)
  usage_rec_t u; buddy_rec_t b; bool hu, hb; (void)u; (void)b; (void)hu; (void)hb;
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd\":\"permission\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":\"req_abc\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"decision\":\"approve\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"v\":1"));
}

static void test_build_permission_deny(void) {
  char buf[128];
  hub_build_permission(buf, sizeof(buf), "req_x", false);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"decision\":\"deny\""));
}

static void test_build_overflow_returns_zero(void) {
  char buf[8];                                     // too small for any valid frame
  TEST_ASSERT_EQUAL_size_t(0, hub_build_permission(buf, sizeof(buf), "req_abc", true));
}

// ===== ack / err =====

static void test_parse_ack_ok(void) {
  const char* j = "{\"v\":1,\"ack\":\"req_abc\",\"ok\":true}";
  hub_ack_t a; memset(&a, 0, sizeof(a));
  TEST_ASSERT_TRUE(hub_parse_ack(j, strlen(j), &a));
  TEST_ASSERT_FALSE(a.is_err);
  TEST_ASSERT_TRUE(a.ok);
  TEST_ASSERT_EQUAL_STRING("req_abc", a.id);
}

static void test_parse_err(void) {
  const char* j = "{\"v\":1,\"err\":\"unknown_prompt_id\",\"id\":\"req_xyz\"}";
  hub_ack_t a; memset(&a, 0, sizeof(a));
  TEST_ASSERT_TRUE(hub_parse_ack(j, strlen(j), &a));
  TEST_ASSERT_TRUE(a.is_err);
  TEST_ASSERT_FALSE(a.ok);
  TEST_ASSERT_EQUAL_STRING("req_xyz", a.id);
}

static void test_parse_ack_not_ok(void) {
  // ok:false = decision did not apply (late/superseded); parses true, is_err=false, ok=false (issue #8).
  const char* j = "{\"v\":1,\"ack\":\"req_abc\",\"ok\":false}";
  hub_ack_t a; memset(&a, 0, sizeof(a));
  TEST_ASSERT_TRUE(hub_parse_ack(j, strlen(j), &a));
  TEST_ASSERT_FALSE(a.is_err);
  TEST_ASSERT_FALSE(a.ok);
  TEST_ASSERT_EQUAL_STRING("req_abc", a.id);
}

static void test_parse_ack_neither(void) {
  const char* j = "{\"v\":1,\"usage\":{}}";        // a status frame is not an ack
  hub_ack_t a; memset(&a, 0, sizeof(a));
  TEST_ASSERT_FALSE(hub_parse_ack(j, strlen(j), &a));
}

// ===== ack state transition (hub_apply_ack, issue #8) =====

static void mk_pending_prompt(buddy_rec_t* b, const char* id) {
  memset(b, 0, sizeof(*b));
  b->prompt.present = true;
  b->prompt.decision_state = PROMPT_PENDING;
  strncpy(b->prompt.id, id, BUDDY_ID_LEN - 1);
}

static void test_apply_ack_ok_holds_prompt(void) {
  buddy_rec_t b; mk_pending_prompt(&b, "req_abc");
  hub_ack_t a = { "req_abc", true, false };
  TEST_ASSERT_TRUE(hub_apply_ack(&b, &a));
  TEST_ASSERT_EQUAL_UINT8(PROMPT_SENT_OK, b.prompt.decision_state);
  TEST_ASSERT_TRUE(b.prompt.present);                   // kept: the device tick clears it after the confirm-hold beat (issue #12)
}

static void test_apply_ack_not_ok_keeps_prompt(void) {
  buddy_rec_t b; mk_pending_prompt(&b, "req_abc");
  hub_ack_t a = { "req_abc", false, false };            // ok:false (late/superseded)
  TEST_ASSERT_TRUE(hub_apply_ack(&b, &a));
  TEST_ASSERT_EQUAL_UINT8(PROMPT_TOO_LATE, b.prompt.decision_state);
  TEST_ASSERT_TRUE(b.prompt.present);                   // kept so the UI can warn, never shows success
}

static void test_apply_ack_err_keeps_prompt(void) {
  buddy_rec_t b; mk_pending_prompt(&b, "req_abc");
  hub_ack_t a = { "req_abc", false, true };             // err frame
  TEST_ASSERT_TRUE(hub_apply_ack(&b, &a));
  TEST_ASSERT_EQUAL_UINT8(PROMPT_TOO_LATE, b.prompt.decision_state);
  TEST_ASSERT_TRUE(b.prompt.present);
}

static void test_apply_ack_mismatched_id_ignored(void) {
  buddy_rec_t b; mk_pending_prompt(&b, "req_abc");
  hub_ack_t a = { "req_other", true, false };
  TEST_ASSERT_FALSE(hub_apply_ack(&b, &a));             // stale id => no change
  TEST_ASSERT_EQUAL_UINT8(PROMPT_PENDING, b.prompt.decision_state);
  TEST_ASSERT_TRUE(b.prompt.present);
}

static void test_apply_ack_not_pending_ignored(void) {
  buddy_rec_t b; mk_pending_prompt(&b, "req_abc");
  b.prompt.decision_state = PROMPT_IDLE_DECISION;       // no decision awaiting an ack
  hub_ack_t a = { "req_abc", true, false };
  TEST_ASSERT_FALSE(hub_apply_ack(&b, &a));
  TEST_ASSERT_TRUE(b.prompt.present);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reassemble_single_frame);
  RUN_TEST(test_reassemble_two_frames_one_feed);
  RUN_TEST(test_reassemble_split_across_feeds);
  RUN_TEST(test_reassemble_crlf_and_empty);
  RUN_TEST(test_reassemble_overflow_then_recover);
  RUN_TEST(test_parse_full_frame);
  RUN_TEST(test_parse_null_and_missing_windows);
  RUN_TEST(test_parse_prompt_absent_is_idle);
  RUN_TEST(test_parse_same_prompt_preserves_pending);
  RUN_TEST(test_parse_new_prompt_resets_decision);
  RUN_TEST(test_parse_truncates_long_strings);
  RUN_TEST(test_parse_entries_capped);
  RUN_TEST(test_parse_rejects_bad_version_and_garbage);
  RUN_TEST(test_build_permission_roundtrips);
  RUN_TEST(test_build_permission_deny);
  RUN_TEST(test_build_overflow_returns_zero);
  RUN_TEST(test_parse_ack_ok);
  RUN_TEST(test_parse_ack_not_ok);
  RUN_TEST(test_parse_err);
  RUN_TEST(test_parse_ack_neither);
  RUN_TEST(test_apply_ack_ok_holds_prompt);
  RUN_TEST(test_apply_ack_not_ok_keeps_prompt);
  RUN_TEST(test_apply_ack_err_keeps_prompt);
  RUN_TEST(test_apply_ack_mismatched_id_ignored);
  RUN_TEST(test_apply_ack_not_pending_ignored);
  return UNITY_END();
}
