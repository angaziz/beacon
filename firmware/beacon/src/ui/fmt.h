#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// Thousands-grouped value. Decimals: 0 for >=1000, else 2 (so 18026 => "18,026", 52.18 => "52.18").
static inline void fmt_value(char* buf, size_t n, double v) {
  int dec = (fabs(v) >= 1000.0) ? 0 : 2;
  char raw[32]; snprintf(raw, sizeof(raw), "%.*f", dec, v);
  char* dot = strchr(raw, '.');
  int int_len = dot ? (int)(dot - raw) : (int)strlen(raw);
  int neg = raw[0] == '-';
  int digits = int_len - neg;
  char out[40]; int o = 0;
  for (int i = 0; i < int_len; i++) {
    int pos = i - neg;                       // digit index within the number
    if (pos > 0 && (digits - pos) % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  if (dot) { while (*dot) out[o++] = *dot++; }
  out[o] = 0;
  snprintf(buf, n, "%s", out);
}

// Signed change: glyph (^ up / v down / - flat) + abs percent, e.g. "^ 0.12%".
// Returns: +1 up, -1 down, 0 flat (caller picks up/down/dim color).
static inline int fmt_change(char* buf, size_t n, double pct) {
  const char* g = pct > 0 ? "^" : (pct < 0 ? "v" : "-");
  snprintf(buf, n, "%s %.2f%%", g, fabs(pct));
  return pct > 0 ? 1 : (pct < 0 ? -1 : 0);
}
