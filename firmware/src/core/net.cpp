#include "core/net.h"
#include "core/nvs.h"
#include "core/provision.h"
#include "core/timekeep.h"
#include "config/root_ca.h"
#include "util/log.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static volatile bool      s_up = false;
static SemaphoreHandle_t  s_tls_mtx = nullptr;
static bool               s_ntp_started = false;
// WiFiMulti is touched ONLY by net_service() (Core-0). The UI/portal mutate the saved list (nvs, under
// its own mutex) + set the dirty flag; net_service rebuilds the AP set + runs the (blocking) reconnect.
static WiFiMulti          s_multi;
static bool               s_sta = false;      // net_begin ran (STA mode active)
static volatile bool      s_enabled = true;   // Connect/Disconnect toggle (pause auto-join)
// Runtime "add network" portal: UI sets s_prov_req (Core-1); net_service (Core-0) owns the AP radio and
// reflects it in s_prov_radio. While provisioning, net issues no STA run()/disconnect (the portal owns
// the radio); provision_loop (Core-1) runs the captive servers off these flags.
static volatile bool      s_prov_req = false;     // Core-1 writer (UI)
static volatile bool      s_prov_radio = false;   // Core-0 writer (net_service)

// Published status snapshot: written by net_service (Core-0), read by the UI (Core-1). Keeps the
// `WiFi` singleton off Core-1 entirely (the §6 DataStore pattern) — no torn reads of WiFi.SSID() etc.
static SemaphoreHandle_t  s_stat_mtx = nullptr;
static struct { bool up; bool enabled; char ssid[33]; char ip[24]; } s_stat = { false, true, "", "" };

static void update_snapshot(void) {
  bool up = net_is_up();
  String ssid = up ? WiFi.SSID() : String();
  String ip   = up ? WiFi.localIP().toString() : String();
  if (s_stat_mtx) xSemaphoreTake(s_stat_mtx, portMAX_DELAY);
  s_stat.up = up; s_stat.enabled = s_enabled;
  strncpy(s_stat.ssid, ssid.c_str(), sizeof(s_stat.ssid) - 1); s_stat.ssid[sizeof(s_stat.ssid) - 1] = 0;
  strncpy(s_stat.ip,   ip.c_str(),   sizeof(s_stat.ip)   - 1); s_stat.ip[sizeof(s_stat.ip)   - 1] = 0;
  if (s_stat_mtx) xSemaphoreGive(s_stat_mtx);
}

// Rebuild the WiFiMulti AP set from the saved list; if the currently-joined SSID is no longer saved
// (forgotten), drop it so the next run() re-evaluates. Core-0 only.
static void rebuild_aps(void) {
  wifi_list_t l; nvs_wifi_snapshot(&l);
  s_multi.APlistClean();
  bool active_still_saved = false;
  String cur = WiFi.SSID();
  for (uint8_t i = 0; i < l.count; i++) {
    s_multi.addAP(l.e[i].ssid, l.e[i].pass);
    if (cur.length() && cur == l.e[i].ssid) active_still_saved = true;
  }
  if (net_is_up() && !active_still_saved) { LOGI("active net forgotten; dropping"); WiFi.disconnect(); }
}

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t /*info*/) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      s_up = true;
      LOGI("wifi up ssid=%s ip=%s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      if (!s_ntp_started) { timekeep_start_ntp(); s_ntp_started = true; }  // SNTP needs the link up
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (s_up) LOGW("wifi down");
      s_up = false;
      // Do NOT WiFi.reconnect() here: WiFiMulti (net_service) owns reconnection, and a manual
      // Disconnect must stay disconnected. setAutoReconnect(false) keeps esp_wifi from re-joining too.
      break;
    default: break;
  }
}

void net_begin(void) {
  s_tls_mtx = xSemaphoreCreateMutex();
  s_stat_mtx = xSemaphoreCreateMutex();
  WiFi.persistent(false);     // we own creds in NVS; keep esp_wifi out of the 20 KB nvs partition
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // WiFiMulti owns (dis)connect/roam decisions
  WiFi.onEvent(on_wifi_event);
  s_sta = true;
  rebuild_aps();              // first net_service() run() will join the strongest saved net
  LOGI("wifi sta up; %u saved network(s)", (unsigned)nvs_wifi_count());
}

// Pumped from the Core-0 fetch task — the ONLY place WiFi (dis)connect/run happens, so the UI never
// touches the WiFi singleton. Applies saved-list changes, honors the toggle, then performs the
// blocking WiFiMulti reconnect only when disconnected + enabled + at least one saved net.
void net_service(void) {
  if (!s_sta) return;

  // Runtime add-network portal: net owns the AP radio (the UI never touches WiFi). Bring the SoftAP up
  // on request; tear it down once the captive servers (Core-1) have stopped, then apply the new creds.
  if (s_prov_req && !s_prov_radio) {
    WiFi.mode(WIFI_AP_STA);            // AP_STA keeps the current STA link up while the portal is open
    WiFi.softAP(provision_ap_ssid());
    s_prov_radio = true;
    LOGI("provision(runtime) AP up");
  } else if (!s_prov_req && s_prov_radio && !provision_servers_up()) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_prov_radio = false;
    nvs_wifi_clear_dirty(); rebuild_aps();   // a net may have been added: refresh WiFiMulti now
    LOGI("provision(runtime) AP down; %u saved", (unsigned)nvs_wifi_count());
  }
  if (s_prov_radio) { update_snapshot(); return; }   // portal owns the radio: no STA run()/disconnect

  if (!s_enabled) {                                  // user Disconnect: drop + stay off (Core-0 disconnect)
    if (net_is_up()) { LOGI("wifi disconnect (user)"); WiFi.disconnect(); }
    update_snapshot();
    return;
  }
  if (nvs_wifi_dirty()) { nvs_wifi_clear_dirty(); rebuild_aps(); }   // clear BEFORE rebuild => no lost update
  if (!net_is_up() && nvs_wifi_count() > 0) s_multi.run(6000);
  update_snapshot();
}

void net_set_enabled(bool en) { s_enabled = en; }   // flag only; net_service (Core-0) acts on it
bool net_is_enabled(void) { return s_enabled; }

void net_request_provision(bool on) { s_prov_req = on; }   // flag only; net_service (Core-0) acts on it
bool net_provision_requested(void)  { return s_prov_req; }
bool net_provision_radio_up(void)   { return s_prov_radio; }

bool net_is_up(void) { return s_up && WiFi.status() == WL_CONNECTED; }

void net_status_str(char* buf, size_t cap) {   // UI (Core-1): reads the published snapshot, not WiFi.*
  bool up, en; char ssid[33], ip[24];
  if (s_stat_mtx) xSemaphoreTake(s_stat_mtx, portMAX_DELAY);
  up = s_stat.up; en = s_stat.enabled;
  strncpy(ssid, s_stat.ssid, sizeof(ssid)); strncpy(ip, s_stat.ip, sizeof(ip));
  if (s_stat_mtx) xSemaphoreGive(s_stat_mtx);
  if (up)                         snprintf(buf, cap, "%s %s", ssid, ip);
  else if (!en)                   snprintf(buf, cap, "OFF");
  else if (nvs_wifi_count() == 0) snprintf(buf, cap, "not set");
  else                            snprintf(buf, cap, "CONNECTING");
}

void net_active_ssid(char* out, size_t cap) {   // UI (Core-1): from the snapshot
  if (!out || !cap) return;
  if (s_stat_mtx) xSemaphoreTake(s_stat_mtx, portMAX_DELAY);
  strncpy(out, s_stat.up ? s_stat.ssid : "", cap - 1); out[cap - 1] = 0;
  if (s_stat_mtx) xSemaphoreGive(s_stat_mtx);
}

static data_err_t map_http(int code) {
  if (code == 429)                 return ERR_RATE_LIMITED;
  if (code >= 200 && code < 300)   return ERR_NONE;
  if (code > 0)                    return ERR_HTTP;
  return ERR_TIMEOUT;   // HTTPClient negative => connection/read failure
}

data_err_t net_https_get(const char* host, const char* path,
                         const char* const* hdr_keys, const char* const* hdr_vals, int hdr_n,
                         char* out, size_t cap, int* status) {
  if (status) *status = 0;
  if (out && cap) out[0] = 0;
  if (!net_is_up()) return ERR_NO_ROUTE;
  if (!s_tls_mtx || xSemaphoreTake(s_tls_mtx, pdMS_TO_TICKS(15000)) != pdTRUE) return ERR_TIMEOUT;

  data_err_t err;
  {
    WiFiClientSecure client;
    client.setCACert(ROOT_CA_BUNDLE);        // cert-validated; never setInsecure() (tech.md §9)
    client.setHandshakeTimeout(12);          // seconds
    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", host, path);
    HTTPClient https;
    https.setConnectTimeout(8000);
    https.setTimeout(8000);
    if (!https.begin(client, url)) { err = ERR_NO_ROUTE; }
    else {
      for (int i = 0; i < hdr_n; i++) https.addHeader(hdr_keys[i], hdr_vals[i]);
      int code = https.GET();
      if (status) *status = code;
      err = map_http(code);
      if (err == ERR_NONE) {
        String body = https.getString();
        if (out && cap) { strncpy(out, body.c_str(), cap - 1); out[cap - 1] = 0; }
        if (out && body.length() >= cap) LOGW("http body truncated host=%s len=%u cap=%u",
                                              host, (unsigned)body.length(), (unsigned)cap);
      } else {
        LOGW("http get host=%s code=%d", host, code);
      }
      https.end();
    }
  }
  xSemaphoreGive(s_tls_mtx);
  return err;
}
