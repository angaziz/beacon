#pragma once
#include <stdint.h>
#include <stddef.h>

// FROZEN (FR-STATE-0). Transport-agnostic hub link; the BLE implementation is P2,
// a LAN-WebSocket fallback may also implement it. Frame reassembly (splitting BLE
// writes on '\n') lives BELOW this interface; onFrame always delivers one whole frame.

// One reassembled, newline-stripped status frame. Called from loop() context (the task
// that pumps HubLink::loop()). `json` is valid ONLY during the callback; copy if retained.
typedef void (*hub_frame_cb)(const char* json, size_t len);

class HubLink {
public:
  virtual ~HubLink() {}
  virtual bool begin() = 0;                  // start transport (advertise / connect)
  virtual bool isConnected() = 0;
  virtual void onFrame(hub_frame_cb cb) = 0; // register the status-frame handler
  // Queue a device->hub command. Returns true if ACCEPTED FOR TRANSPORT (enqueued);
  // false if not connected OR the send queue is full. NOT the application-level ack
  // (the hub acks separately, tech.md §7.1) — only reports local enqueue. The
  // implementation MUST copy `json` before returning; the caller may reuse/free it after.
  virtual bool send(const char* json, size_t len) = 0;
  // Drain already-queued send() bytes to the transport NOW, rather than waiting for the next loop().
  // A caller that enqueues several frames in one pass (e.g. the chunked ticker report, issue #106) calls
  // this between frames so the bounded send buffer cannot overflow. No-op by default; only a buffering
  // transport overrides it. Like loop()/the drain, this must run on the Core-0 pump task.
  virtual void flush() {}
  virtual void loop() = 0;                    // pumped from a Core-0 task
};
