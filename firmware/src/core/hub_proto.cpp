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
      buddy->prompt.queue_len = 1;
    } else {
      const char* pid = p["id"].as<const char*>();
      // A full-frame resend (e.g. on reconnect) can repeat the SAME prompt we are already showing;
      // preserve its in-flight confirm state (PENDING/TOO_LATE) so a pending ack can still land (#8).
      // Only a genuinely new id (or a prompt shown after idle) is fresh to decide.
      bool same_prompt = buddy->prompt.present && pid && strcmp(buddy->prompt.id, pid) == 0;
      if (!same_prompt) buddy->prompt.decision_state = PROMPT_IDLE_DECISION;
      buddy->prompt.present = true;
      copy_trunc(buddy->prompt.id,   BUDDY_ID_LEN,   pid);
      copy_trunc(buddy->prompt.tool, BUDDY_TOOL_LEN, p["tool"].as<const char*>());
      copy_trunc(buddy->prompt.hint, BUDDY_HINT_LEN, p["hint"].as<const char*>());
      int q = p["qlen"] | 1;                                 // absent/non-numeric => 1
      buddy->prompt.queue_len = (uint8_t)(q < 1 ? 1 : (q > 255 ? 255 : q));  // clamp: 0/neg => 1, cap at 255
    }
  }
  return true;
}

bool hub_parse_loc(const char* json, size_t len, hub_loc_t* out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;   // not valid JSON
  if ((doc["v"] | 0) != 1) return false;               // unknown major version => ignore
  JsonVariantConst l = doc["loc"];
  if (l.isNull()) return false;                         // no loc block in this frame
  out->lat = l["lat"] | 0.0f;
  out->lon = l["lon"] | 0.0f;
  copy_trunc(out->tz,   sizeof(out->tz),   l["tz"].as<const char*>());
  copy_trunc(out->name, sizeof(out->name), l["name"].as<const char*>());
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
    p->decision_state = PROMPT_SENT_OK;                                   // keep present: the device tick clears it after the confirm-hold beat
  } else {
    p->decision_state = PROMPT_TOO_LATE;                                  // keep present so the UI can warn
  }
  return true;
}

// --- config chunk: wire enum string => firmware enum ---
// Returns false (leaving *out untouched) on an unknown string so the caller can ack the right bad_*.
static bool map_source(const char* s, ticker_source_t* out) {
  if (!s) return false;
  if (!strcmp(s, "binance")) { *out = SRC_BINANCE; return true; }
  if (!strcmp(s, "yahoo"))   { *out = SRC_YAHOO;   return true; }
  return false;
}
static bool map_kind(const char* s, ticker_kind_t* out) {
  if (!s) return false;
  if (!strcmp(s, "fx"))     { *out = KIND_FX;     return true; }
  if (!strcmp(s, "crypto")) { *out = KIND_CRYPTO; return true; }
  if (!strcmp(s, "index"))  { *out = KIND_INDEX;  return true; }
  if (!strcmp(s, "etf"))    { *out = KIND_ETF;    return true; }
  return false;
}
static bool map_basis(const char* s, change_basis_t* out) {
  if (!s) return false;
  if (!strcmp(s, "prev_close")) { *out = CHG_PREV_CLOSE; return true; }
  if (!strcmp(s, "24h"))        { *out = CHG_24H;        return true; }
  return false;
}

// Copy src into dst[cap] only if it fits (<= cap-1, +NUL); false otherwise (no silent truncation: the
// spec rejects over-length id/sym/name with "malformed", design §3.4). NULL/empty handled by caller.
static bool copy_bounded(char* dst, size_t cap, const char* src) {
  size_t n = strlen(src);
  if (n > cap - 1) return false;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return true;
}

#define CFG_SET_ERR(s) do { if (err_out) *err_out = (s); } while (0)

data_err_t hub_parse_config_chunk(const char* json, size_t len, config_chunk_t* out,
                                  const char** err_out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) { CFG_SET_ERR("malformed"); return ERR_PARSE; }
  if ((doc["v"] | 0) != 1)            { CFG_SET_ERR("malformed"); return ERR_PARSE; }

  JsonVariantConst cfg = doc["config"];
  JsonVariantConst tickers = cfg["tickers"];
  if (cfg.isNull() || !tickers.is<JsonArrayConst>()) { CFG_SET_ERR("malformed"); return ERR_PARSE; }

  // parts must be a positive total; part a valid 0-based index within it.
  int parts = cfg["parts"] | 0;
  int part  = cfg["part"]  | -1;
  if (parts <= 0 || part < 0 || part >= parts) { CFG_SET_ERR("malformed"); return ERR_PARSE; }
  out->rev   = cfg["rev"] | (uint32_t)0;
  out->part  = part;
  out->parts = parts;

  out->row_count = 0;
  for (JsonVariantConst row : tickers.as<JsonArrayConst>()) {
    if (out->row_count >= MAX_TICKERS) { CFG_SET_ERR("malformed"); return ERR_PARSE; }  // accumulator re-checks totals
    ticker_runtime_t* r = &out->rows[out->row_count];

    const char* id = row["id"].as<const char*>();
    if (!id || id[0] == '\0' || !copy_bounded(r->id, FIN_ID_LEN, id)) {  // id required + non-empty
      CFG_SET_ERR("malformed"); return ERR_PARSE;
    }
    const char* sym = row["sym"].as<const char*>();
    const char* name = row["name"].as<const char*>();
    if (!sym || !name || !copy_bounded(r->symbol, TKR_SYM_LEN, sym) ||
        !copy_bounded(r->name, TKR_NAME_LEN, name)) {
      CFG_SET_ERR("malformed"); return ERR_PARSE;
    }
    if (!map_source(row["src"].as<const char*>(), &r->source)) { CFG_SET_ERR("bad_source"); return ERR_PARSE; }
    if (!map_kind(row["kind"].as<const char*>(), &r->kind))    { CFG_SET_ERR("bad_kind");   return ERR_PARSE; }
    if (!map_basis(row["basis"].as<const char*>(), &r->change_basis)) { CFG_SET_ERR("bad_basis"); return ERR_PARSE; }
    r->cadence_s = (uint16_t)(row["cadence"] | 0);
    r->stale_s   = row["stale"] | (uint32_t)0;
    out->row_count++;
  }
  return ERR_NONE;
}

config_status_t hub_config_accum_step(config_accum_t* acc, const config_chunk_t* chunk,
                                      const char** err_out) {
  if (chunk->part == 0) {                 // part 0 always (re)starts a fresh accumulation
    acc->active    = true;
    acc->rev       = chunk->rev;
    acc->parts     = chunk->parts;
    acc->next_part = 0;
    acc->row_count = 0;
  } else if (!acc->active || chunk->rev != acc->rev || chunk->parts != acc->parts ||
             chunk->part != acc->next_part) {
    acc->active = false;                   // discard partial: out-of-order/duplicate/rev mismatch/gap
    CFG_SET_ERR("bad_chunking");
    return CFG_ERR;
  }

  if (acc->row_count + chunk->row_count > MAX_TICKERS) {
    acc->active = false;
    CFG_SET_ERR("too_many_tickers");
    return CFG_ERR;
  }
  for (int i = 0; i < chunk->row_count; i++) acc->rows[acc->row_count++] = chunk->rows[i];
  acc->next_part = chunk->part + 1;

  if (acc->next_part < acc->parts) return CFG_PENDING;   // more parts to come

  acc->active = false;                    // snapshot complete (success or empty both end accumulation)
  if (acc->row_count < 1) { CFG_SET_ERR("empty"); return CFG_ERR; }
  return CFG_DONE;
}

#undef CFG_SET_ERR

size_t hub_build_config_ack(char* buf, size_t cap, uint32_t rev, bool ok, const char* err, int count) {
  if (!buf || cap == 0) return 0;
  JsonDocument doc;
  doc["v"] = 1;
  doc["cmd"] = "config_ack";
  doc["rev"] = rev;
  doc["ok"] = ok;
  if (ok) doc["count"] = count;
  else    doc["err"] = err ? err : "malformed";
  return finish_frame(doc, buf, cap);
}

// --- device -> hub ticker report (issue #105) ---
#define REPORT_CHUNK_MAX 900   // mirror ConfigFrame maxBytes; margin under HUB_FRAME_MAX

static const char* src_to_wire(ticker_source_t s) {
  switch (s) { case SRC_BINANCE: return "binance"; case SRC_YAHOO: return "yahoo"; }
  return NULL;
}
static const char* kind_to_wire(ticker_kind_t k) {
  switch (k) {
    case KIND_FX: return "fx"; case KIND_CRYPTO: return "crypto";
    case KIND_INDEX: return "index"; case KIND_ETF: return "etf";
  }
  return NULL;
}
static const char* basis_to_wire(change_basis_t b) {
  switch (b) { case CHG_PREV_CLOSE: return "prev_close"; case CHG_24H: return "24h"; }
  return NULL;
}

size_t hub_build_report_frame(const ticker_runtime_t* rows, int lo, int hi,
                              int part, int parts, char* buf, size_t cap) {
  if (!rows || !buf || cap == 0) return 0;
  if (parts <= 0 || part < 0 || part >= parts) return 0;            // bad chunk coordinates
  if (lo < 0 || hi <= lo || hi > MAX_TICKERS) return 0;             // bad / out-of-bounds range
  JsonDocument doc;
  doc["v"]    = 1;
  doc["cmd"]  = "report";
  doc["what"] = "tickers";
  doc["rev"]  = 0;
  doc["part"] = part;
  doc["parts"] = parts;
  JsonArray arr = doc["tickers"].to<JsonArray>();
  for (int i = lo; i < hi; i++) {
    const char* src   = src_to_wire(rows[i].source);
    const char* kind  = kind_to_wire(rows[i].kind);
    const char* basis = basis_to_wire(rows[i].change_basis);
    if (!src || !kind || !basis) return 0;          // unmappable enum => fail closed (all-or-zero)
    JsonObject o = arr.add<JsonObject>();
    o["id"]      = rows[i].id;
    o["src"]     = src;
    o["sym"]     = rows[i].symbol;
    o["name"]    = rows[i].name;
    o["kind"]    = kind;
    o["cadence"] = rows[i].cadence_s;
    o["stale"]   = rows[i].stale_s;
    o["basis"]   = basis;
  }
  return finish_frame(doc, buf, cap);
}

int hub_report_plan(const ticker_runtime_t* rows, int count, int group_start[MAX_TICKERS]) {
  if (!rows || !group_start || count < 1 || count > MAX_TICKERS) return 0;
  // Measure with worst-case header digits: parts=count AND part=count-1 (the real part/parts are both
  // <= these, so a group that fits the measurement still fits when re-serialized with the real values).
  const int wp = count - 1;
  int groups = 0, lo = 0;
  for (int i = 0; i < count; i++) {
    char tmp[HUB_FRAME_MAX];
    size_t n = hub_build_report_frame(rows, lo, i + 1, wp, count, tmp, sizeof(tmp));
    // n==0 when the probe overflowed HUB_FRAME_MAX (definitely >REPORT_CHUNK_MAX) or an enum error.
    // Both are treated as "over budget" — the split logic below disambiguates the i==lo failure.
    bool over = (n == 0 || n > REPORT_CHUNK_MAX);
    if (!over) continue;                             // row i still fits the current group
    if (i == lo) return 0;                          // a single row alone exceeds the budget (or enum error)
    group_start[groups++] = lo;                     // close [lo..i)
    lo = i;                                          // row i opens a new group
    size_t n2 = hub_build_report_frame(rows, lo, i + 1, wp, count, tmp, sizeof(tmp));
    if (n2 == 0 || n2 > REPORT_CHUNK_MAX) return 0; // row i alone over budget
  }
  group_start[groups++] = lo;                       // final open group
  return groups;
}
