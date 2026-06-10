#include "ui/wifi_panel.h"
#include "ui/theme.h"
#include "ui/carousel.h"
#include "config/layout.h"
#include "core/net.h"
#include "core/nvs.h"
#include "core/provision.h"
#include <lvgl.h>
#include <string.h>

static lv_obj_t*   s_root    = nullptr;   // full-screen modal on lv_layer_top
static lv_obj_t*   s_status  = nullptr;
static lv_obj_t*   s_list    = nullptr;
static lv_obj_t*   s_toggle  = nullptr;
static lv_obj_t*   s_add     = nullptr;   // add-network portal sub-overlay (or null)
static lv_obj_t*   s_add_status = nullptr;
static bool        s_add_done = false;    // latch the "added" state so the tick stops overwriting it
static lv_obj_t*   s_confirm = nullptr;   // forget-confirm sub-overlay (or null)
static lv_timer_t* s_timer   = nullptr;

static char        s_row_ssid[WIFI_MAX_SAVED][WIFI_SSID_CAP];
static lv_obj_t*   s_row_dot[WIFI_MAX_SAVED];
static uint8_t     s_row_n = 0;

static void build_list(void);
static void close_panel(void);

static void mklabel(lv_obj_t* o, const lv_font_t* f, lv_color_t c) {
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_set_style_text_color(o, c, 0);
  lv_obj_set_style_text_letter_space(o, 2, 0);
}

// --- forget confirm ---------------------------------------------------------
static void confirm_dismiss(void) { if (s_confirm) { lv_obj_del(s_confirm); s_confirm = nullptr; } }
static void confirm_cancel_cb(lv_event_t*) { confirm_dismiss(); }
static void confirm_forget_cb(lv_event_t* e) {
  const char* ssid = (const char*)lv_event_get_user_data(e);
  nvs_wifi_forget(ssid);     // net_service (Core-0) rebuilds WiFiMulti + drops it if active
  confirm_dismiss();
  build_list();
}

static void open_confirm(const char* ssid) {
  const beacon_theme_t* t = theme_active();
  confirm_dismiss();
  s_confirm = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_confirm);
  lv_obj_set_size(s_confirm, SCREEN_W, SCREEN_H);
  lv_obj_center(s_confirm);
  lv_obj_set_style_bg_color(s_confirm, t->bg, 0);
  lv_obj_set_style_bg_opa(s_confirm, LV_OPA_90, 0);
  lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);   // eat taps behind it

  static char held[WIFI_SSID_CAP];   // stable for the callback
  strncpy(held, ssid, sizeof(held) - 1); held[sizeof(held) - 1] = 0;

  lv_obj_t* q = lv_label_create(s_confirm); mklabel(q, t->f_body, t->ink_dim);
  lv_label_set_text(q, "forget network"); lv_obj_align(q, LV_ALIGN_CENTER, 0, -42);
  lv_obj_t* n = lv_label_create(s_confirm); mklabel(n, t->f_display, t->ink);
  lv_label_set_text(n, held); lv_obj_align(n, LV_ALIGN_CENTER, 0, -16);

  lv_obj_t* cancel = lv_label_create(s_confirm); mklabel(cancel, t->f_body, t->ink_dim);
  lv_label_set_text(cancel, "CANCEL"); lv_obj_align(cancel, LV_ALIGN_CENTER, -56, 30);
  lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(cancel, 18);
  lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* forget = lv_label_create(s_confirm); mklabel(forget, t->f_body, t->down);
  lv_label_set_text(forget, "FORGET"); lv_obj_align(forget, LV_ALIGN_CENTER, 56, 30);
  lv_obj_add_flag(forget, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(forget, 18);
  lv_obj_add_event_cb(forget, confirm_forget_cb, LV_EVENT_CLICKED, held);
}

static void row_cb(lv_event_t* e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < s_row_n) open_confirm(s_row_ssid[idx]);
}

// --- list -------------------------------------------------------------------
static void build_list(void) {
  const beacon_theme_t* t = theme_active();
  lv_obj_clean(s_list);
  s_row_n = nvs_wifi_count();
  if (s_row_n > WIFI_MAX_SAVED) s_row_n = WIFI_MAX_SAVED;
  if (s_row_n == 0) {
    lv_obj_t* empty = lv_label_create(s_list); mklabel(empty, t->f_body, t->ink_dim);
    lv_label_set_text(empty, "no saved networks");
    return;
  }
  for (uint8_t i = 0; i < s_row_n; i++) {
    nvs_wifi_get_ssid(i, s_row_ssid[i], WIFI_SSID_CAP);
    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 40);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(dot, t->line, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);
    s_row_dot[i] = dot;

    lv_obj_t* nm = lv_label_create(row); mklabel(nm, t->f_display, t->ink);
    lv_label_set_text(nm, s_row_ssid[i]); lv_obj_align(nm, LV_ALIGN_LEFT_MID, 22, 0);
  }
}

// --- live refresh (status + active dot) -------------------------------------
static void refresh(void) {
  const beacon_theme_t* t = theme_active();
  char st[48]; net_status_str(st, sizeof(st));
  lv_label_set_text(s_status, st);
  lv_label_set_text(s_toggle, net_is_enabled() ? "DISCONNECT" : "CONNECT");
  char active[WIFI_SSID_CAP]; net_active_ssid(active, sizeof(active));
  for (uint8_t i = 0; i < s_row_n; i++)
    lv_obj_set_style_bg_color(s_row_dot[i], (active[0] && strcmp(active, s_row_ssid[i]) == 0) ? t->accent : t->line, 0);
}

// --- add network (on-demand setup portal, no reboot) ------------------------
static void add_dismiss(void) {
  if (!s_add) return;
  net_request_provision(false);        // tear the AP down if the user closes before saving
  provision_runtime_clear_saved();
  lv_obj_del(s_add); s_add = nullptr; s_add_status = nullptr;
}
static void add_close_cb(lv_event_t*) { add_dismiss(); build_list(); refresh(); }

// Driven by the 1 s panel tick while the overlay is open: net (Core-0) brings the AP up within ~1 s,
// then the user joins it and submits a network on their phone; provision_runtime_saved() flips on save.
static void add_tick(void) {
  if (!s_add || s_add_done) return;
  const beacon_theme_t* t = theme_active();
  if (provision_runtime_saved()) {
    lv_label_set_text(s_add_status, "added - you can leave Beacon-setup");
    lv_obj_set_style_text_color(s_add_status, t->accent, 0);
    net_request_provision(false);      // done: tear the portal down (creds already in NVS)
    provision_runtime_clear_saved();
    s_add_done = true;
    build_list();                      // reflect the new network in the list behind the overlay
    return;
  }
  lv_label_set_text(s_add_status, provision_active() ? "join \"Beacon-setup\", then enter your Wi-Fi"
                                                     : "starting setup...");
}

static void open_add(void) {
  if (s_add) return;
  const beacon_theme_t* t = theme_active();
  provision_runtime_clear_saved();
  net_request_provision(true);         // net (Core-0) owns the radio; brings the AP up
  s_add_done = false;

  s_add = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_add);
  lv_obj_set_size(s_add, SCREEN_W, SCREEN_H);
  lv_obj_center(s_add);
  lv_obj_set_style_bg_color(s_add, t->bg, 0);
  lv_obj_set_style_bg_opa(s_add, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_add, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_add, LV_OBJ_FLAG_CLICKABLE);   // eat taps behind it

  lv_obj_t* hdr = lv_label_create(s_add); mklabel(hdr, t->f_body, t->ink_dim);
  lv_label_set_text(hdr, "ADD NETWORK"); lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);

  lv_obj_t* step = lv_label_create(s_add); mklabel(step, t->f_body, t->ink_dim);
  lv_label_set_text(step, "on a phone, open Wi-Fi settings and join:");
  lv_obj_align(step, LV_ALIGN_CENTER, 0, -46);

  lv_obj_t* ap = lv_label_create(s_add); mklabel(ap, t->f_display, t->accent);
  lv_label_set_text(ap, provision_ap_ssid()); lv_obj_align(ap, LV_ALIGN_CENTER, 0, -20);

  s_add_status = lv_label_create(s_add); mklabel(s_add_status, t->f_body, t->ink);
  lv_label_set_long_mode(s_add_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_add_status, SCREEN_W - 4 * SAFE_INSET);
  lv_obj_set_style_text_align(s_add_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(s_add_status, "starting setup..."); lv_obj_align(s_add_status, LV_ALIGN_CENTER, 0, 26);

  lv_obj_t* close = lv_label_create(s_add); mklabel(close, t->f_body, t->ink_dim);
  lv_label_set_text(close, "CLOSE"); lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);
  lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(close, 24);
  lv_obj_add_event_cb(close, add_close_cb, LV_EVENT_CLICKED, NULL);
}

static void tick_cb(lv_timer_t*) { refresh(); add_tick(); }

// --- controls ---------------------------------------------------------------
static void back_cb(lv_event_t*)   { close_panel(); }
static void toggle_cb(lv_event_t*) { net_set_enabled(!net_is_enabled()); refresh(); }
static void add_cb(lv_event_t*)    { open_add(); }

static void close_panel(void) {
  if (!s_root) return;
  if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
  if (s_add) net_request_provision(false);   // never leave the setup AP up after the panel closes
  lv_obj_del(s_root);                         // also deletes the s_add / s_confirm children
  s_root = s_status = s_list = s_toggle = s_confirm = nullptr;
  s_add = s_add_status = nullptr;
  s_row_n = 0;
  carousel_set_swipe_enabled(true);   // unconditional restore — never leave the carousel frozen
}

void wifi_panel_open(void) {
  if (s_root) return;                 // already open (idempotent)
  const beacon_theme_t* t = theme_active();
  carousel_set_swipe_enabled(false);

  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
  lv_obj_center(s_root);
  lv_obj_set_style_bg_color(s_root, t->bg, 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);   // capture taps so none leak to the carousel

  lv_obj_t* back = lv_label_create(s_root); mklabel(back, t->f_body, t->ink_dim);
  lv_label_set_text(back, "< back"); lv_obj_align(back, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(back, 24);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* title = lv_label_create(s_root); mklabel(title, t->f_body, t->ink_dim);
  lv_label_set_text(title, "WI-FI"); lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -SAFE_INSET, SAFE_INSET);

  s_status = lv_label_create(s_root); mklabel(s_status, t->f_body, t->ink);
  lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, SAFE_INSET, SAFE_INSET + 26);

  s_list = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, SCREEN_W - 2 * SAFE_INSET, SCREEN_H - 2 * SAFE_INSET - 124);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, SAFE_INSET + 60);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_row(s_list, 2, 0);

  lv_obj_t* add = lv_label_create(s_root); mklabel(add, t->f_body, t->accent);
  lv_label_set_text(add, "+ add network");
  lv_obj_align(add, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET - 30);
  lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(add, 18);
  lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, NULL);

  s_toggle = lv_label_create(s_root); mklabel(s_toggle, t->f_body, t->ink);
  lv_obj_align(s_toggle, LV_ALIGN_BOTTOM_MID, 0, -SAFE_INSET);
  lv_obj_add_flag(s_toggle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_toggle, 24);
  lv_obj_add_event_cb(s_toggle, toggle_cb, LV_EVENT_CLICKED, NULL);

  build_list();
  refresh();
  s_timer = lv_timer_create(tick_cb, 1000, NULL);
}

bool wifi_panel_is_open(void) { return s_root != nullptr; }

// Public wrapper: allows external callers (e.g. IMU shake dispatcher) to close.
// lv_obj_del(s_root) in close_panel() also destroys s_add and s_confirm children,
// so all sub-cards are fully torn down by this call.
void wifi_panel_close(void) { close_panel(); }
