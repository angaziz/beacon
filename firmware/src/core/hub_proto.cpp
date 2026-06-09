#include "core/hub_proto.h"
#include <ArduinoJson.h>
#include <string.h>

// Copy a C string into a fixed dst[cap], truncating to cap-1 + NUL (records.h string rule).
static void copy_trunc(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strnlen(src, cap - 1);
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void hub_reassembler_reset(hub_reassembler_t* r) {
  r->len = 0; r->overflow = false; r->drops = 0;
}

void hub_reassembler_feed(hub_reassembler_t* r, const char* data, size_t n,
                          hub_line_cb cb, void* user) {
  for (size_t i = 0; i < n; i++) {
    char c = data[i];
    if (c == '\n') {                       // frame boundary
      if (r->overflow) { r->drops++; r->overflow = false; r->len = 0; continue; }
      size_t len = r->len;
      if (len > 0 && r->buf[len - 1] == '\r') len--;   // tolerate CRLF
      if (len > 0) { r->buf[len] = '\0'; cb(r->buf, len, user); }   // skip empty frames
      r->len = 0;
      continue;
    }
    if (r->overflow) continue;             // discard the rest of an oversize frame
    if (r->len >= HUB_FRAME_MAX - 1) { r->overflow = true; continue; }
    r->buf[r->len++] = c;
  }
}

// A window object {"pct":..,"reset":..}; absent/null pct => -1 (unavailable), absent reset => 0.
static void fill_window(usage_window_t* w, JsonVariantConst v) {
  JsonVariantConst p = v["pct"];
  w->pct   = p.isNull() ? -1 : (int16_t)p.as<int>();
  w->reset = v["reset"] | (uint32_t)0;
}

bool hub_parse_status(const char* json, size_t len,
                      usage_rec_t* usage, bool* had_usage,
                      buddy_rec_t* buddy, bool* had_buddy) {
  *had_usage = false; *had_buddy = false;
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;   // not valid JSON
  if ((doc["v"] | 0) != 1) return false;               // unknown major version => ignore (tech.md §7.1)

  JsonVariantConst u = doc["usage"];
  if (!u.isNull()) {
    *had_usage = true;
    fill_window(&usage->claude.h5, u["claude"]["h5"]);  // missing provider => its windows go null
    fill_window(&usage->claude.d7, u["claude"]["d7"]);
    fill_window(&usage->codex.h5,  u["codex"]["h5"]);
    fill_window(&usage->codex.d7,  u["codex"]["d7"]);
  }

  JsonVariantConst b = doc["buddy"];
  if (!b.isNull()) {
    *had_buddy = true;
    buddy->running     = (uint8_t)(b["running"]     | 0);
    buddy->waiting     = (uint8_t)(b["waiting"]     | 0);
    buddy->tokens      = b["tokens"]                 | (uint32_t)0;
    buddy->context_pct = (uint8_t)(b["context_pct"] | 0);

    buddy->entry_count = 0;
    for (JsonVariantConst e : b["entries"].as<JsonArrayConst>()) {
      if (buddy->entry_count >= BUDDY_ENTRIES) break;
      copy_trunc(buddy->entries[buddy->entry_count], BUDDY_ENTRY_LEN, e.as<const char*>());
      buddy->entry_count++;
    }

    JsonVariantConst p = b["prompt"];
    if (p.isNull()) {                                   // absent prompt => idle
      buddy->prompt.present = false;
      buddy->prompt.id[0] = buddy->prompt.tool[0] = buddy->prompt.hint[0] = '\0';
    } else {
      buddy->prompt.present = true;
      buddy->prompt.decision_state = PROMPT_IDLE_DECISION;   // a (re)shown prompt is fresh to decide (issue #8)
      copy_trunc(buddy->prompt.id,   BUDDY_ID_LEN,   p["id"].as<const char*>());
      copy_trunc(buddy->prompt.tool, BUDDY_TOOL_LEN, p["tool"].as<const char*>());
      copy_trunc(buddy->prompt.hint, BUDDY_HINT_LEN, p["hint"].as<const char*>());
    }
  }
  return true;
}

// Serialize `doc` into buf as a newline-terminated frame. Returns bytes (incl. '\n', excl. NUL) or 0.
static size_t finish_frame(JsonDocument& doc, char* buf, size_t cap) {
  size_t need = measureJson(doc);
  if (need + 2 > cap) return 0;            // no room for the JSON + '\n' + NUL
  size_t n = serializeJson(doc, buf, cap);
  buf[n++] = '\n';
  buf[n] = '\0';
  return n;
}

size_t hub_build_permission(char* buf, size_t cap, const char* id, bool approve) {
  if (!buf || !id || cap == 0) return 0;
  JsonDocument doc;
  doc["v"] = 1;
  doc["cmd"] = "permission";
  doc["id"] = id;
  doc["decision"] = approve ? "approve" : "deny";
  return finish_frame(doc, buf, cap);
}

bool hub_parse_ack(const char* json, size_t len, hub_ack_t* out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if ((doc["v"] | 0) != 1) return false;
  JsonVariantConst ack = doc["ack"];
  if (!ack.isNull()) {                     // {"ack":"<id>","ok":true}
    out->is_err = false;
    out->ok = doc["ok"] | false;
    copy_trunc(out->id, BUDDY_ID_LEN, ack.as<const char*>());
    return true;
  }
  JsonVariantConst err = doc["err"];
  if (!err.isNull()) {                     // {"err":"<reason>","id":"<id>"}
    out->is_err = true;
    out->ok = false;
    copy_trunc(out->id, BUDDY_ID_LEN, doc["id"].as<const char*>());
    return true;
  }
  return false;
}

bool hub_apply_ack(buddy_rec_t* buddy, const hub_ack_t* ack) {
  buddy_prompt_t* p = &buddy->prompt;
  if (!p->present || p->decision_state != PROMPT_PENDING) return false;   // nothing awaiting an ack
  if (strncmp(p->id, ack->id, BUDDY_ID_LEN) != 0) return false;           // stale/mismatched id
  if (ack->ok && !ack->is_err) {
    p->decision_state = PROMPT_SENT_OK;
    p->present = false;                                                   // applied => clear the prompt
  } else {
    p->decision_state = PROMPT_TOO_LATE;                                  // keep present so the UI can warn
  }
  return true;
}
