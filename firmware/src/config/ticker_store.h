#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config/ticker_table.h"   // ticker_runtime_t, MAX_TICKERS bounds

// Versioned packed-blob codec + NVS persistence for the runtime ticker table (design §3.2).
//
// source/kind/basis are persisted as STABLE WIRE CODES, never raw C enum ordinals: the enum
// ordinals can shift (e.g. SRC_FRANKFURTER was removed), and a blob keyed on ordinals would
// silently misread after such a shift. The codes below are frozen and decoupled from the enums.
//   src:   1=binance 2=yahoo
//   kind:  1=fx 2=crypto 3=index 4=etf
//   basis: 1=prev_close 2=24h
//
// Blob layout (little-endian):
//   [u8 schema_ver=1][u8 count][u32 crc32 of body][body: count * packed-entry]
// Packed entry (fixed, NUL-padded fields use the ticker_table.h length bounds):
//   id[FIN_ID_LEN] symbol[TKR_SYM_LEN] name[TKR_NAME_LEN]
//   [u8 src_code][u8 kind_code][u8 basis_code][u16 cadence_s][u32 stale_s]
#define TICKER_STORE_SCHEMA_VER 1

#ifdef __cplusplus
extern "C" {
#endif

// crc32 with the standard reflected polynomial 0xEDB88320 (zlib/PNG).
uint32_t ticker_store_crc32(const uint8_t* data, size_t len);

// PURE codec (host-testable; no NVS).
size_t ticker_store_pack(const ticker_runtime_t* rows, int count, uint8_t* buf, size_t cap);
// bytes written, 0 on overflow / bad count
int    ticker_store_unpack(const uint8_t* buf, size_t len, ticker_runtime_t* out, int max);
// count, or -1 if invalid (bad ver/crc/len, count out of [1,max], or unknown src/kind/basis code)

// Worst-case packed blob size, for stack buffers.
#define TICKER_ENTRY_BYTES (FIN_ID_LEN + TKR_SYM_LEN + TKR_NAME_LEN + 1 + 1 + 1 + 2 + 4)
#define TICKER_BLOB_MAX    (1 + 1 + 4 + MAX_TICKERS * TICKER_ENTRY_BYTES)

// DEVICE I/O wrappers (NVS-backed; no-op stubs on the native host where Preferences is absent).
bool ticker_store_save(const ticker_runtime_t* rows, int count);  // pack + NVS write; false on write failure
int  ticker_store_load(ticker_runtime_t* out, int max);           // NVS read + unpack; -1 if absent/invalid

#ifdef __cplusplus
}
#endif
