#include "core/provision.h"
#include "core/net.h"
#include "core/nvs.h"
#include "util/log.h"
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

static const char* AP_SSID = "Beacon-setup";
static DNSServer    s_dns;
static WebServer    s_server(80);
static bool         s_active = false;
static bool         s_restart_pending = false;
static uint32_t     s_restart_at = 0;
static String       s_options;   // <option> list, scanned ONCE at begin (cheap to serve per request)

// Runtime portal (no reboot): the AP radio is owned by net (Core-0); the DNS/HTTP servers live here on
// Core-1. s_runtime distinguishes it from the boot path so handle_save persists without rebooting.
static bool         s_runtime = false;       // Core-1: runtime servers are running (vs. boot portal)
static volatile bool s_runtime_saved = false; // Core-1 writer (handle_save); read+cleared by the UI

// Escape an SSID before it lands in the value="..." attribute below: a nearby AP can name itself with
// quotes/angle-brackets/ampersands, which would otherwise break the form or inject markup on the portal.
static String html_attr_escape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += "&amp;";  break;
      case '"': o += "&quot;"; break;
      case '<': o += "&lt;";   break;
      case '>': o += "&gt;";   break;
      default:  o += c;
    }
  }
  return o;
}

// Scan visible networks once and cache them as datalist <option>s. Done at begin(), not per request,
// so serving the page on every captive-probe stays fast (a scan blocks ~2 s).
static void scan_networks(void) {
  int n = WiFi.scanNetworks();
  s_options = "";
  for (int i = 0; i < n && i < 20; i++)
    s_options += "<option value=\"" + html_attr_escape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
}

// Setup page: scanned-network datalist + manual SSID/password, posting to /save.
static String page_html(void) {
  return String(
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Beacon setup</title>"
    "<style>body{font-family:system-ui;background:#000;color:#eee;margin:0;padding:24px}"
    "h1{font-size:20px}input{width:100%;padding:12px;margin:6px 0;font-size:16px;"
    "background:#111;color:#eee;border:1px solid #333;border-radius:8px;box-sizing:border-box}"
    "button{width:100%;padding:14px;font-size:16px;background:#3a7;color:#000;border:0;border-radius:8px}</style>"
    "<h1>Beacon WiFi setup</h1>"
    "<form method=POST action=/save>"
    "<input name=ssid list=nets placeholder='Network name (SSID)' autocomplete=off required>"
    "<datalist id=nets>") + s_options + String(
    "</datalist>"
    "<input name=pass type=password placeholder='Password' autocomplete=off>"
    "<button type=submit>") + (s_runtime ? "Save" : "Save &amp; reboot") + String("</button></form>");
}

// Add security headers before every HTML response: Cache-Control prevents the
// browser from caching a page that carries the password field; nosniff blocks
// MIME-type confusion. Minimal overhead for an embedded captive portal.
static void send_html(int code, const String& body) {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.sendHeader("X-Content-Type-Options", "nosniff");
  s_server.send(code, "text/html", body);
}

static void handle_root(void) { send_html(200, page_html()); }

static void handle_save(void) {
  String ssid = s_server.arg("ssid");
  String pass = s_server.arg("pass");
  if (ssid.length() == 0) { send_html(400, "<h2>SSID required</h2>"); return; }
  // IEEE 802.11 SSID max is 32 bytes; WPA2 credentials are either a passphrase
  // (8-63 ASCII chars) or a 64-hex-char raw PSK. WIFI_PASS_CAP is 65 (64 + NUL).
  if (ssid.length() > 32) { send_html(400, "<h2>SSID too long (32 char max)</h2>"); return; }
  if (pass.length() > 64) { send_html(400, "<h2>Password too long (64 char max)</h2>"); return; }
  if (!nvs_wifi_add(ssid.c_str(), pass.c_str())) {   // append (dedup); creds never logged
    send_html(200, "<h2>Saved networks full</h2><p>Forget one on the device first.</p>");
    return;
  }
  if (s_runtime) {   // on-demand add: NVS dirty flag is set; net applies it on teardown — no reboot
    LOGI("provision(runtime) saved ssid=%s; no reboot", ssid.c_str());
    send_html(200, "<h2>Saved.</h2><p>You can leave Beacon-setup now.</p>");
    s_runtime_saved = true;   // UI polls this and tears the portal down via net_request_provision(false)
    return;
  }
  LOGI("provision saved ssid=%s; rebooting to STA", ssid.c_str());
  send_html(200, "<h2>Saved. Rebooting...</h2>");
  s_restart_pending = true;
  s_restart_at = millis() + 1500;
}

// Captive-portal catch-all: serve the form (200) for EVERY unhandled path. The OS connectivity probe
// (captive.apple.com, generate_204, connecttest.txt) thus gets the portal instead of the expected
// success page, so macOS/iOS/Android/Windows auto-open their captive login window.
static void handle_notfound(void) { send_html(200, page_html()); }

// Register HTTP routes once: WebServer.on() appends a handler per call, so doing this on every runtime
// open/close cycle would leak. Guarded so boot and runtime share one registration.
static void ensure_routes(void) {
  static bool done = false;
  if (done) return;
  s_server.on("/", handle_root);
  s_server.on("/save", HTTP_POST, handle_save);
  s_server.onNotFound(handle_notfound);
  done = true;
}

bool provision_needed(void) { return !nvs_has_wifi(); }

void provision_begin(void) {
  WiFi.mode(WIFI_AP_STA);     // AP for the portal; STA side enables the network scan
  WiFi.softAP(AP_SSID);
  feedLoopWDT();               // scan_networks() blocks ~2-4s; give the loop-task WDT (setup() runs on
                                // it, per main.cpp's enableLoopWDT()) a fresh window before it (issue: a
                                // first-boot/post-erase device with no saved WiFi hits this path from
                                // setup() and was tripping the WDT here, panic-looping before the portal
                                // ever came up)
  scan_networks();            // cache the visible-network list once
  IPAddress ip = WiFi.softAPIP();
  s_dns.start(53, "*", ip);   // resolve every name to the portal (captive)
  ensure_routes();
  s_server.begin();
  s_active = true;
  LOGI("provision AP=%s ip=%s", AP_SSID, ip.toString().c_str());
}

// volatile: this is the cross-core teardown guard — net (Core-0) must not drop the AP radio until it
// observes this false (after Core-1 has stopped the servers). Single Core-1 writer, Core-0 reader.
static volatile bool s_servers_up = false;

// Runtime portal servers (Core-1). The AP radio is already up (net, Core-0). No network scan: it blocks
// ~2 s and would stall the LVGL loop here — the user types the SSID into the portal's manual field.
static void runtime_servers_start(void) {
  s_options = "";
  // SoftAP default gateway IP (we never call softAPConfig). Hardcoded so Core-1 never reads the WiFi
  // singleton — net (Core-0) owns it. Captive DNS resolves every name to this address.
  IPAddress ip(192, 168, 4, 1);
  s_dns.start(53, "*", ip);
  ensure_routes();
  s_server.begin();
  s_servers_up = true;
  s_active = true;
  s_runtime = true;
  LOGI("provision(runtime) servers up ip=%s", ip.toString().c_str());
}

static void runtime_servers_stop(void) {
  s_server.stop();
  s_dns.stop();
  s_servers_up = false;
  s_active = false;
  s_runtime = false;
  LOGI("provision(runtime) servers down");
}

void provision_loop(void) {
  // Boot path: provision_begin() owns the AP on Core-1; reboot after a successful save.
  if (s_active && !s_runtime) {
    s_dns.processNextRequest();
    s_server.handleClient();
    if (s_restart_pending && (int32_t)(millis() - s_restart_at) >= 0) { delay(200); ESP.restart(); }
    return;
  }
  // Runtime path: net (Core-0) owns the AP radio; mirror the captive servers to its state.
  bool radio = net_provision_radio_up();
  bool want  = net_provision_requested();
  if (radio && want && !s_servers_up)      runtime_servers_start();
  if (s_servers_up && (!want || !radio))   runtime_servers_stop();
  if (s_servers_up) { s_dns.processNextRequest(); s_server.handleClient(); }
}

bool        provision_active(void)  { return s_active; }
const char* provision_ap_ssid(void) { return AP_SSID; }
bool        provision_servers_up(void)          { return s_servers_up; }
bool        provision_runtime_saved(void)       { return s_runtime_saved; }
void        provision_runtime_clear_saved(void) { s_runtime_saved = false; }
