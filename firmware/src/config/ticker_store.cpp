#include "config/ticker_store.h"
#include <string.h>

// --- stable code <-> enum mapping ---------------------------------------------------------
// Switch-based, NOT a cast: the wire codes are frozen independently of the C enum ordinals, so a
// future enum reorder/removal cannot silently misread a persisted blob. Unknown codes => reject.

static uint8_t src_to_code(ticker_source_t s) {
  switch (s) { case SRC_BINANCE: return 1; case SRC_YAHOO: return 2; default: return 0; }
}
static bool code_to_src(uint8_t c, ticker_source_t* out) {
  switch (c) { case 1: *out = SRC_BINANCE; return true; case 2: *out = SRC_YAHOO; return true; default: return false; }
}
static uint8_t kind_to_code(ticker_kind_t k) {
  switch (k) { case KIND_FX: return 1; case KIND_CRYPTO: return 2; case KIND_INDEX: return 3; case KIND_ETF: return 4; default: return 0; }
}
static bool code_to_kind(uint8_t c, ticker_kind_t* out) {
  switch (c) { case 1: *out = KIND_FX; return true; case 2: *out = KIND_CRYPTO; return true;
               case 3: *out = KIND_INDEX; return true; case 4: *out = KIND_ETF; return true; default: return false; }
}
static uint8_t basis_to_code(change_basis_t b) {
  switch (b) { case CHG_PREV_CLOSE: return 1; case CHG_24H: return 2; default: return 0; }
}
static bool code_to_basis(uint8_t c, change_basis_t* out) {
  switch (c) { case 1: *out = CHG_PREV_CLOSE; return true; case 2: *out = CHG_24H; return true; default: return false; }
}

uint32_t ticker_store_crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// little-endian field writers/readers
static void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32(uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
static uint16_t get_u16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get_u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#define HDR_BYTES 6   // u8 ver + u8 count + u32 crc

size_t ticker_store_pack(const ticker_runtime_t* rows, int count, uint8_t* buf, size_t cap) {
  if (!rows || !buf || count < 1 || count > MAX_TICKERS) return 0;
  size_t need = HDR_BYTES + (size_t)count * TICKER_ENTRY_BYTES;
  if (cap < need) return 0;

  buf[0] = TICKER_STORE_SCHEMA_VER;
  buf[1] = (uint8_t)count;
  uint8_t* body = buf + HDR_BYTES;
  uint8_t* p = body;
  for (int i = 0; i < count; i++) {
    const ticker_runtime_t* t = &rows[i];
    memset(p, 0, FIN_ID_LEN);   memcpy(p, t->id, strnlen(t->id, FIN_ID_LEN - 1));     p += FIN_ID_LEN;
    memset(p, 0, TKR_SYM_LEN);  memcpy(p, t->symbol, strnlen(t->symbol, TKR_SYM_LEN - 1)); p += TKR_SYM_LEN;
    memset(p, 0, TKR_NAME_LEN); memcpy(p, t->name, strnlen(t->name, TKR_NAME_LEN - 1));     p += TKR_NAME_LEN;
    *p++ = src_to_code(t->source);
    *p++ = kind_to_code(t->kind);
    *p++ = basis_to_code(t->change_basis);
    put_u16(p, t->cadence_s); p += 2;
    put_u32(p, t->stale_s);   p += 4;
  }
  put_u32(buf + 2, ticker_store_crc32(body, (size_t)(p - body)));
  return need;
}

int ticker_store_unpack(const uint8_t* buf, size_t len, ticker_runtime_t* out, int max) {
  if (!buf || !out || len < HDR_BYTES) return -1;
  if (buf[0] != TICKER_STORE_SCHEMA_VER) return -1;
  int count = buf[1];
  if (count < 1 || count > MAX_TICKERS || count > max) return -1;

  size_t body_len = (size_t)count * TICKER_ENTRY_BYTES;
  if (len != HDR_BYTES + body_len) return -1;               // truncated or oversize

  const uint8_t* body = buf + HDR_BYTES;
  if (ticker_store_crc32(body, body_len) != get_u32(buf + 2)) return -1;

  const uint8_t* p = body;
  for (int i = 0; i < count; i++) {
    ticker_runtime_t* t = &out[i];
    memset(t, 0, sizeof(*t));
    // fixed fields are NUL-padded; force a terminator in case the stored field is unterminated.
    memcpy(t->id, p, FIN_ID_LEN);     t->id[FIN_ID_LEN - 1] = 0;     p += FIN_ID_LEN;
    memcpy(t->symbol, p, TKR_SYM_LEN); t->symbol[TKR_SYM_LEN - 1] = 0; p += TKR_SYM_LEN;
    memcpy(t->name, p, TKR_NAME_LEN);  t->name[TKR_NAME_LEN - 1] = 0;  p += TKR_NAME_LEN;
    if (!code_to_src(*p++, &t->source)) return -1;
    if (!code_to_kind(*p++, &t->kind)) return -1;
    if (!code_to_basis(*p++, &t->change_basis)) return -1;
    t->cadence_s = get_u16(p); p += 2;
    t->stale_s   = get_u32(p); p += 4;
  }
  return count;
}

// --- device I/O ---------------------------------------------------------------------------
// NVS-backed on the device; Preferences is absent from the native test build, so stub there.
#if !BEACON_NATIVE
#include "core/nvs.h"

static const char* TICKER_NVS_KEY = "tkrcfg";

bool ticker_store_save(const ticker_runtime_t* rows, int count) {
  uint8_t buf[TICKER_BLOB_MAX];
  size_t n = ticker_store_pack(rows, count, buf, sizeof(buf));
  if (n == 0) return false;
  return nvs_set_bytes(TICKER_NVS_KEY, buf, n);   // CHECK the write result (existing code ignored it)
}

int ticker_store_load(ticker_runtime_t* out, int max) {
  uint8_t buf[TICKER_BLOB_MAX];
  size_t n = nvs_get_bytes(TICKER_NVS_KEY, buf, sizeof(buf));
  if (n == 0) return -1;                           // absent or oversize
  return ticker_store_unpack(buf, n, out, max);
}
#else
bool ticker_store_save(const ticker_runtime_t*, int) { return false; }
int  ticker_store_load(ticker_runtime_t*, int)       { return -1; }
#endif
