#include "core/about_format.h"
#include <stdio.h>

void about_fmt_mac(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void about_fmt_uptime(uint32_t secs, char* out, size_t cap) {
  uint32_t mins = secs / 60;
  if (mins < 60)        snprintf(out, cap, "%um", (unsigned)mins);
  else if (mins < 1440) snprintf(out, cap, "%uh %um", (unsigned)(mins / 60), (unsigned)(mins % 60));
  else                  snprintf(out, cap, "%ud %uh", (unsigned)(mins / 1440), (unsigned)((mins % 1440) / 60));
}

void about_fmt_heap_kb(uint32_t bytes, char* out, size_t cap) {
  snprintf(out, cap, "%u KB", (unsigned)(bytes / 1024));
}
