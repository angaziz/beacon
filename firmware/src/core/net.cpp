#include "core/net.h"
#include "core/nvs.h"
#include "core/provision.h"
#include "core/timekeep.h"
#include "config/root_ca.h"
#include "util/log.h"
#include <Arduino.h>
#include <string.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static volatile bool      s_up = false;
static SemaphoreHandle_t  s_tls_mtx = nullptr;
static bool               s_ntp_started = false;

// Persistent HTTPS connection reused across consecutive same-host fetches (#61). A fresh
// WiFiClientSecure per fetch re-ran a ~40-50KB mbedtls handshake every ~25-30s -- the classic
// internal-heap fragmentation pattern. The fetch task drains same-host due slots back-to-back
// (see fetch_task.cpp), so one handshake serves a whole sweep; net_close_idle frees the socket (and
// its mbedtls context) between sweeps. All access is from the single Core-0 fetch task.
static WiFiClientSecure   s_cli;
static HTTPClient         s_http;
static char               s_host[80] = {0};
static bool               s_open = false;
static uint32_t           s_open_ms = 0;     // millis() of last use, for the idle close
static uint32_t           s_handshakes = 0;  // cumulative; logged so a sweep's handshake count is verifiable
#define NET_IDLE_CLOSE_MS 8000u
#define NET_BODY_DEADLINE_MS 8000u   // overall body-read budget; the cooperative drain yields, so this
                                     // only bounds a half-dead socket -- it is not a WDT mitigation

// caller holds s_tls_mtx (or is the sole fetch-task path before the lock is contended)
static void net_conn_close(void) {
  if (!s_open) return;
  s_http.end();    // with reuse this only detaches; stop() actually closes the TLS/TCP socket
  s_cli.stop();
  s_open = false;
  s_host[0] = 0;
}

// --- cooperative HTTP body drain (#92) -----------------------------------------------------
// HTTPClient::writeToStream drains via Stream::timedRead, a busy-spin with no yield; on a multi-second
// body stall it starved IDLE0 on the Core-0 fetch task and tripped the 5s task WDT (panic + reboot).
// These helpers read straight off s_cli, gating every read on available() and vTaskDelay-ing IDLE0 when
// the socket has no data -- so the WDT stays fed however slowly the body arrives. An overall deadline
// (NET_BODY_DEADLINE_MS) bounds a half-dead keep-alive socket; the per-read yield, not the deadline, is
// the WDT fix. Content-Length and Transfer-Encoding: chunked are the only shapes our HTTP/1.1 hosts use.

// Copy n body bytes into the caller buffer; flag overflow but keep draining so the keep-alive socket
// stays clean (#61). The caller NUL-terminates out once the whole body is drained.
static void body_emit(char* out, size_t cap, size_t* outlen, bool* over, const uint8_t* d, int n) {
  if (!out || !cap) return;
  size_t room = (*outlen < cap - 1) ? (cap - 1 - *outlen) : 0;
  size_t w = ((size_t)n < room) ? (size_t)n : room;
  if (w) memcpy(out + *outlen, d, w);
  *outlen += w;
  if (w < (size_t)n) *over = true;
}

// Read one chunk-size line ("<hex>[;ext]\r\n"). Returns the size (>= 0; 0 = last chunk) or -1 on stall.
static long read_chunk_size(uint32_t deadline) {
  long sz = 0; bool digit = false, in_ext = false;
  for (;;) {
    if ((int32_t)(millis() - deadline) >= 0 || !s_cli.connected()) return -1;
    uint8_t b; if (s_cli.read(&b, 1) <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    if (b == '\n') return digit ? sz : -1;
    if (b == '\r' || in_ext) continue;
    if (b == ';') { in_ext = true; continue; }   // chunk extension: ignore to end of line
    int v = (b >= '0' && b <= '9') ? b - '0'
          : (b >= 'a' && b <= 'f') ? b - 'a' + 10
          : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
    if (v >= 0) { sz = sz * 16 + v; digit = true; }
  }
}

// Copy exactly n_bytes of body off the socket into out. Returns false on stall/disconnect/deadline.
static bool read_body_bytes(int n_bytes, uint32_t deadline, char* out, size_t cap,
                            size_t* outlen, bool* over) {
  uint8_t tmp[256];
  while (n_bytes > 0) {
    if ((int32_t)(millis() - deadline) >= 0 || !s_cli.connected()) return false;
    if (s_cli.available() <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }   // no data yet: yield IDLE0
    int want = n_bytes < (int)sizeof(tmp) ? n_bytes : (int)sizeof(tmp);
    int n = s_cli.read(tmp, (size_t)want);
    if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    body_emit(out, cap, outlen, over, tmp, n);
    n_bytes -= n;
  }
  return true;
}

// After the terminating 0-size chunk, drain the trailer section (lines through the final empty CRLF) so
// the keep-alive socket is positioned at the next response. Returns false on stall.
static bool consume_trailers(uint32_t deadline) {
  for (;;) {
    bool any = false;
    for (;;) {
      if ((int32_t)(millis() - deadline) >= 0 || !s_cli.connected()) return false;
      uint8_t b; if (s_cli.read(&b, 1) <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
      if (b == '\n') break;
      if (b != '\r') any = true;
    }
    if (!any) return true;   // empty line => end of trailers
  }
}

// Drain the response body cooperatively. len = Content-Length (>= 0) or -1 for chunked. Writes up to
// cap-1 bytes into out (NUL-terminated); sets *truncated if the body exceeded the buffer. Returns
// ERR_NONE on a complete body, ERR_TIMEOUT on a stall/short read (caller drops the socket).
static data_err_t drain_body(int len, char* out, size_t cap, bool* truncated) {
  size_t outlen = 0; bool over = false;
  uint32_t deadline = millis() + NET_BODY_DEADLINE_MS;
  data_err_t rc = ERR_NONE;

  if (len >= 0) {
    if (!read_body_bytes(len, deadline, out, cap, &outlen, &over)) rc = ERR_TIMEOUT;
  } else {
    for (;;) {   // chunked: <hex>\r\n <data> \r\n ... 0\r\n [trailers] \r\n
      long sz = read_chunk_size(deadline);
      if (sz < 0) { rc = ERR_TIMEOUT; break; }
      if (sz == 0) { if (!consume_trailers(deadline)) rc = ERR_TIMEOUT; break; }
      if (!read_body_bytes((int)sz, deadline, out, cap, &outlen, &over)) { rc = ERR_TIMEOUT; break; }
      for (int got = 0; got < 2; ) {   // consume the CRLF trailing each chunk's data
        if ((int32_t)(millis() - deadline) >= 0 || !s_cli.connected()) { rc = ERR_TIMEOUT; break; }
        uint8_t b; if (s_cli.read(&b, 1) <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        got++;
      }
      if (rc != ERR_NONE) break;
    }
  }

  if (out && cap) out[outlen] = 0;
  if (truncated) *truncated = over;
  return rc;
}
// WiFiMulti is touched ONLY by net_service() (Core-0). The UI/portal mutate the saved list (nvs, under
// its own mutex) + set the dirty flag; net_service rebuilds the AP set + runs the (blocking) reconnect.
static WiFiMulti          s_multi;
static bool               s_sta = false;      // net_begin ran (STA mode active)
static volatile bool      s_enabled = true;   // Connect/Disconnect toggle (pause auto-join)
// Reconnect backoff (#62): WiFiMulti::run() blocks ~2-6s (scan + join). While the saved net is
// unreachable, gate it behind an exponential retry (5s/15s/60s cap) instead of scanning back-to-back
// every ~1s fetch-task tick. Reset to "attempt now" on success, a creds change, or a Connect toggle.
static uint32_t           s_next_join_ms = 0;   // do not run() before this millis(); 0 = attempt now
static uint8_t            s_join_fails   = 0;    // consecutive failed joins => backoff step
static void net_join_reset(void) { s_join_fails = 0; s_next_join_ms = 0; }
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
  static bool snap_up = false; static bool snap_en = true; static uint8_t n = 0;
  // #65 L1: WiFi.SSID()/localIP() each alloc an Arduino String; rebuild ssid/ip only on a state edge
  // or every ~10s (ssid/ip change only via reconnect, which flips `up` first anyway).
  if (up == snap_up && s_enabled == snap_en && (++n % 10 != 0)) return;
  snap_up = up; snap_en = s_enabled;
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
    nvs_wifi_clear_dirty(); rebuild_aps(); net_join_reset();   // a net may have been added: try it now
    LOGI("provision(runtime) AP down; %u saved", (unsigned)nvs_wifi_count());
  }
  if (s_prov_radio) { update_snapshot(); return; }   // portal owns the radio: no STA run()/disconnect

  if (!s_enabled) {                                  // user Disconnect: drop + stay off (Core-0 disconnect)
    if (net_is_up()) { LOGI("wifi disconnect (user)"); WiFi.disconnect(); }
    net_join_reset();                                // a later Connect should retry immediately
    update_snapshot();
    return;
  }
  if (nvs_wifi_dirty()) { nvs_wifi_clear_dirty(); rebuild_aps(); net_join_reset(); }   // new creds: retry now
  if (!net_is_up() && nvs_wifi_count() > 0 && (int32_t)(millis() - s_next_join_ms) >= 0) {
    LOGI("wifi join attempt (fails=%u)", (unsigned)s_join_fails);
    s_multi.run(6000);
    if (net_is_up()) {
      net_join_reset();                              // connected: clear the backoff
    } else {
      uint32_t d = s_join_fails == 0 ? 5000u : s_join_fails == 1 ? 15000u : 60000u;
      if (s_join_fails < 2) s_join_fails++;
      s_next_join_ms = millis() + d;                 // re-read millis: run() blocked for seconds
      LOGW("wifi join failed; retry in %us", (unsigned)(d / 1000));
    }
  }
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
  char url[256];
  snprintf(url, sizeof(url), "https://%s%s", host, path);

  // Reuse the open socket only for the same host while it is still connected; otherwise tear it down
  // and hand back a fresh one (the handshake then runs inside GET()).
  bool reuse = s_open && s_cli.connected() && strcmp(s_host, host) == 0;
  if (!reuse) {
    net_conn_close();
    s_cli.setCACert(ROOT_CA_BUNDLE);       // cert-validated; never setInsecure() (tech.md §9)
    s_cli.setHandshakeTimeout(4);          // seconds; must stay < the 5s task WDT (see below)
    strncpy(s_host, host, sizeof(s_host) - 1); s_host[sizeof(s_host) - 1] = 0;
  }
  s_http.setReuse(true);
  // Every blocking phase must finish under the 5s Task WDT: a stalled fetch (esp. the body-read poll
  // in writeToStream) that ran past 5s starved IDLE0 and panicked Core 0 (issue #92). Keep all < 5000.
  s_http.setConnectTimeout(4000);
  s_http.setTimeout(4000);
  if (!s_http.begin(s_cli, url)) {
    net_conn_close();
    err = ERR_NO_ROUTE;
  } else {
    for (int i = 0; i < hdr_n; i++) s_http.addHeader(hdr_keys[i], hdr_vals[i]);
    if (!reuse) { s_handshakes++; LOGI("tls handshake host=%s total=%u", host, (unsigned)s_handshakes); }
    int code = s_http.GET();
    if (status) *status = code;
    err = map_http(code);
    if (err == ERR_NONE) {
      // Cooperative drain (see drain_body, #92): yields IDLE0 so a slow/stalled body never starves the
      // Core-0 task WDT, unlike HTTPClient::writeToStream's busy-spin. getSize() is the Content-Length,
      // or -1 for a chunked body (Open-Meteo); both are handled.
      bool truncated = false;
      err = drain_body(s_http.getSize(), out, cap, &truncated);
      if (err != ERR_NONE) { net_conn_close(); LOGW("http body stalled host=%s", host); }   // suspect: force a fresh socket
      else if (truncated) LOGW("http body truncated host=%s cap=%u", host, (unsigned)cap);
    } else {
      LOGW("http get host=%s code=%d", host, code);
    }
    s_http.end();    // reuse=true keeps the socket alive for the next same-host call
    if (s_cli.connected()) { s_open = true; s_open_ms = millis(); }
    else { s_open = false; s_host[0] = 0; }
  }
  xSemaphoreGive(s_tls_mtx);
  return err;
}

// Fetch task: drop the kept-alive socket once a sweep is done so its mbedtls context frees between
// sweeps (a long-idle keep-alive would be closed server-side anyway). Non-blocking; skips if contended.
void net_close_idle(void) {
  if (!s_open) return;
  if (!s_tls_mtx || xSemaphoreTake(s_tls_mtx, 0) != pdTRUE) return;
  if (s_open && (uint32_t)(millis() - s_open_ms) > NET_IDLE_CLOSE_MS) net_conn_close();
  xSemaphoreGive(s_tls_mtx);
}

// Fetch task: the host of the currently-open connection ("" if none), so the scheduler can drain
// same-host due slots over the one open socket. Read only from the fetch task (no cross-core race).
const char* net_open_host(void) { return (s_open && s_cli.connected()) ? s_host : ""; }
