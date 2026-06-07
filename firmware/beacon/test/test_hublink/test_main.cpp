#include <unity.h>
#include <string.h>
#include "core/hublink.h"

// FakeHub proves the frozen interface is implementable and exercises the frozen
// send()/onFrame semantics (enqueue-not-ack, copy-before-return, connected gating).
class FakeHub : public HubLink {
public:
  bool connected = false;
  hub_frame_cb cb = nullptr;
  static const int CAP = 2;
  char queue[CAP][64];
  int qlen = 0;

  bool begin() override { connected = true; return true; }
  bool isConnected() override { return connected; }
  void onFrame(hub_frame_cb c) override { cb = c; }
  bool send(const char* json, size_t len) override {
    if (!connected) return false;          // gated on connection
    if (qlen >= CAP) return false;         // queue full
    strncpy(queue[qlen], json, sizeof(queue[0]) - 1);  // COPY before returning
    queue[qlen][sizeof(queue[0]) - 1] = 0;
    qlen++;
    return true;
  }
  void loop() override {}
  // test helper: simulate a hub->device frame
  void inject(const char* json) { if (cb) cb(json, strlen(json)); }
};

static char g_last_frame[64];
static void capture(const char* json, size_t len) {
  (void)len; strncpy(g_last_frame, json, sizeof(g_last_frame) - 1);
  g_last_frame[sizeof(g_last_frame) - 1] = 0;
}

void setUp(void) { g_last_frame[0] = 0; }
void tearDown(void) {}

static void test_send_gated_on_connection(void) {
  FakeHub h;
  TEST_ASSERT_FALSE(h.send("x", 1));   // disconnected => false
  h.begin();
  TEST_ASSERT_TRUE(h.send("x", 1));    // connected => enqueued
}

static void test_send_copies_buffer(void) {
  FakeHub h; h.begin();
  char buf[8]; strcpy(buf, "hello");
  TEST_ASSERT_TRUE(h.send(buf, 5));
  strcpy(buf, "GONE");                 // caller mutates after send
  TEST_ASSERT_EQUAL_STRING("hello", h.queue[0]);  // hub kept its own copy
}

static void test_send_queue_full(void) {
  FakeHub h; h.begin();
  TEST_ASSERT_TRUE(h.send("a", 1));
  TEST_ASSERT_TRUE(h.send("b", 1));
  TEST_ASSERT_FALSE(h.send("c", 1));   // CAP=2 => third rejected
}

static void test_onframe_delivers(void) {
  FakeHub h; h.begin();
  h.onFrame(capture);
  h.inject("{\"v\":1}");
  TEST_ASSERT_EQUAL_STRING("{\"v\":1}", g_last_frame);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_send_gated_on_connection);
  RUN_TEST(test_send_copies_buffer);
  RUN_TEST(test_send_queue_full);
  RUN_TEST(test_onframe_delivers);
  return UNITY_END();
}
