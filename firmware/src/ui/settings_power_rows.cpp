#include "ui/settings_power_rows.h"
#include "ui/duration_panel.h"
#include "ui/durations.h"
#include "ui/idle_glue.h"
#include "core/nvs.h"
#include <string.h>

static void copy_label(char* out, size_t cap, uint8_t idx) {
  if (idx >= DURATION_COUNT) idx = 0;
  strncpy(out, DURATIONS[idx].label, cap - 1);
  out[cap - 1] = 0;
}

static void on_dim(uint8_t idx)   { nvs_set_dim_idx(idx);   idle_apply_config_from_nvs(); }
static void on_sleep(uint8_t idx) { nvs_set_sleep_idx(idx); idle_apply_config_from_nvs(); }

void settings_power_open_dim(void) {
  duration_panel_open("DIM AFTER", nvs_get_dim_idx(DURATION_DEFAULT_DIM), on_dim);
}
void settings_power_open_sleep(void) {
  duration_panel_open("SLEEP AFTER", nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP), on_sleep);
}
void settings_power_dim_label(char* out, size_t cap)   { copy_label(out, cap, nvs_get_dim_idx(DURATION_DEFAULT_DIM)); }
void settings_power_sleep_label(char* out, size_t cap) { copy_label(out, cap, nvs_get_sleep_idx(DURATION_DEFAULT_SLEEP)); }
