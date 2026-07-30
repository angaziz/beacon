#include "ui/settings_power_rows.h"
#include "ui/duration_panel.h"
#include "ui/durations.h"
#include "ui/idle_glue.h"
#include "core/buddy_wake.h"
#include "core/nvs.h"
#include <stdio.h>

// Indexed by buddy_wake_mode_t; append-only alongside the enum.
static const char* const WAKE_LABELS[BUDDY_WAKE_MODE_COUNT] = {
  "Off", "Prompts", "Attention", "Everything",
};

// Trailing " >" mirrors the Theme row: signals the row opens a list of options.
static void copy_label(char* out, size_t cap, const char* label) {
  snprintf(out, cap, "%s >", label);
}

static uint8_t wake_idx(void) {
  uint8_t v = nvs_get_wake_mode(BUDDY_WAKE_MODE_DEFAULT);
  return v < BUDDY_WAKE_MODE_COUNT ? v : (uint8_t)BUDDY_WAKE_MODE_DEFAULT;
}

static void on_dim(uint8_t idx)   { nvs_set_dim_idx(idx);   idle_apply_config_from_nvs(); }
static void on_sleep(uint8_t idx) { nvs_set_sleep_idx(idx); idle_apply_config_from_nvs(); }
static void on_wake(uint8_t idx)  { nvs_set_wake_mode(idx); idle_apply_config_from_nvs(); }

void settings_power_open_dim(void) {
  duration_panel_open("DIM AFTER", nvs_get_dim_idx(DURATION_DEFAULT_DIM), on_dim);
}
void settings_power_open_sleep(void) {
  duration_panel_open("SLEEP AFTER", nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP), on_sleep);
}
void settings_power_open_wake(void) {
  option_panel_open("WAKE ON", WAKE_LABELS, BUDDY_WAKE_MODE_COUNT, wake_idx(), on_wake);
}

static uint8_t dur_idx(uint8_t v) { return v < DURATION_COUNT ? v : 0; }
void settings_power_dim_label(char* out, size_t cap) {
  copy_label(out, cap, DURATIONS[dur_idx(nvs_get_dim_idx(DURATION_DEFAULT_DIM))].label);
}
void settings_power_sleep_label(char* out, size_t cap) {
  copy_label(out, cap, DURATIONS[dur_idx(nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP))].label);
}
void settings_power_wake_label(char* out, size_t cap) {
  copy_label(out, cap, WAKE_LABELS[wake_idx()]);
}
