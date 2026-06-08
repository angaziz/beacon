#pragma once
// Pure carousel index helpers (LVGL-free, host-tested).
// carousel_clamp/next/prev: bounded, NO wraparound.
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

// Recycling carousel mapping: the active screen is pinned to a fixed center slot, with its
// circular neighbours always present in adjacent slots, so every swipe (including the wrap
// boundary) is an ordinary one-page move. center_slot = count/2.
static inline int carousel_mod(int a, int n) {        // positive modulo (a may be negative)
  if (n <= 0) return 0;
  int m = a % n; return m < 0 ? m + n : m;
}
static inline int carousel_center_slot(int count) { return count / 2; }
// Logical screen shown at physical slot `slot` when `current` sits at the center slot.
static inline int carousel_logical_at(int current, int slot, int count) {
  return carousel_mod(current + slot - carousel_center_slot(count), count);
}
// Inverse: physical slot a logical screen occupies when `current` is centered.
static inline int carousel_slot_of(int current, int logical, int count) {
  return carousel_mod(logical - current + carousel_center_slot(count), count);
}
