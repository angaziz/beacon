#pragma once
// Pure carousel index helpers (LVGL-free, host-tested). Clamp policy: NO wraparound.
static inline int carousel_clamp(int idx, int count) {
  if (count <= 0) return 0;
  if (idx < 0) return 0;
  if (idx >= count) return count - 1;
  return idx;
}
static inline int carousel_next(int idx, int count) { return carousel_clamp(idx + 1, count); }
static inline int carousel_prev(int idx, int count) { return carousel_clamp(idx - 1, count); }
// Nearest page index for a horizontal scroll offset, given page width and gap-free pages.
static inline int carousel_index_for_x(int scroll_x, int page_w, int count) {
  if (page_w <= 0) return 0;
  return carousel_clamp((scroll_x + page_w / 2) / page_w, count);
}
