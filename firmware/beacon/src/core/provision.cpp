#include "core/provision.h"
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
    "<button type=submit>Save &amp; reboot</button></form>");
}

static void handle_root(void) { s_server.send(200, "text/html", page_html()); }

static void handle_save(void) {
  String ssid = s_server.arg("ssid");
  String pass = s_server.arg("pass");
  if (ssid.length() == 0) { s_server.send(400, "text/html", "<h2>SSID required</h2>"); return; }
  if (!nvs_wifi_add(ssid.c_str(), pass.c_str())) {   // append (dedup); creds never logged
    s_server.send(200, "text/html", "<h2>Saved networks full</h2><p>Forget one on the device first.</p>");
    return;
  }
  LOGI("provision saved ssid=%s; rebooting to STA", ssid.c_str());
  s_server.send(200, "text/html", "<h2>Saved. Rebooting...</h2>");
  s_restart_pending = true;
  s_restart_at = millis() + 1500;
}

// Captive-portal catch-all: serve the form (200) for EVERY unhandled path. The OS connectivity probe
// (captive.apple.com, generate_204, connecttest.txt) thus gets the portal instead of the expected
// success page, so macOS/iOS/Android/Windows auto-open their captive login window.
static void handle_notfound(void) { s_server.send(200, "text/html", page_html()); }

bool provision_needed(void) { return !nvs_has_wifi(); }

void provision_begin(void) {
  WiFi.mode(WIFI_AP_STA);     // AP for the portal; STA side enables the network scan
  WiFi.softAP(AP_SSID);
  scan_networks();            // cache the visible-network list once
  IPAddress ip = WiFi.softAPIP();
  s_dns.start(53, "*", ip);   // resolve every name to the portal (captive)
  s_server.on("/", handle_root);
  s_server.on("/save", HTTP_POST, handle_save);
  s_server.onNotFound(handle_notfound);
  s_server.begin();
  s_active = true;
  LOGI("provision AP=%s ip=%s", AP_SSID, ip.toString().c_str());
}

void provision_loop(void) {
  if (!s_active) return;
  s_dns.processNextRequest();
  s_server.handleClient();
  if (s_restart_pending && (int32_t)(millis() - s_restart_at) >= 0) { delay(200); ESP.restart(); }
}

bool        provision_active(void)  { return s_active; }
const char* provision_ap_ssid(void) { return AP_SSID; }
