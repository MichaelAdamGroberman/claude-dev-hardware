#include "net_wifi.h"
#include "net_wg.h"
#include "ble_bridge.h"
#include "stats.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

static WebServer _portal(80);
static bool      _portalRunning = false;
static char      _ip[24]      = "(off)";
static char      _bridge[40]  = "";
static char      _lastErr[48] = "";
static NetWifiState _state    = NW_OFF;

// ── HTML form served at 192.168.4.1 in AP mode ──────────────────────
// Inline so we don't need LittleFS for serving. Dark theme + crimson
// accent matches the gr0m identity.
static const char HTML_FORM[] = R"H(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>gr0m wifi</title>
<style>
body{font:14px -apple-system,system-ui,sans-serif;max-width:380px;margin:32px auto;padding:20px;background:#0c0c0c;color:#e8e8e8}
h1{color:#ff2c20;margin:0 0 6px;letter-spacing:0.04em;font-weight:700}
p{color:#888;font-size:13px;margin:4px 0 18px}
label{display:block;font-size:11px;color:#888;text-transform:uppercase;letter-spacing:0.08em;margin-top:14px;margin-bottom:4px}
input,textarea{width:100%;padding:11px;background:#1a1a1a;border:1px solid #333;color:#e8e8e8;border-radius:3px;box-sizing:border-box;font:14px monospace}
input:focus,textarea:focus{outline:none;border-color:#ff2c20}
textarea{resize:vertical}
button{background:#ff2c20;color:#000;border:none;padding:13px 28px;font-weight:700;cursor:pointer;border-radius:3px;margin-top:20px;font-size:14px;letter-spacing:0.04em}
button:hover{background:#ff5040}
.bolt{display:inline-block;color:#ffe040;text-shadow:0 0 6px rgba(255,224,64,.6)}
small{color:#555;font-size:11px}
</style></head><body>
<h1><span class="bolt">&#9889;</span> gr0m</h1>
<p>configure wifi uplink</p>
<form method="POST" action="/save">
  <label>ssid</label>
  <input name="ssid" placeholder="phone hotspot or home network" autofocus>
  <label>password</label>
  <input name="pwd" type="password" placeholder="network password">
  <label>mac bridge (optional, stage 2+)</label>
  <input name="bridge" placeholder="192.168.1.20:6400">
  <label>wireguard / tailnet config (optional)</label>
  <textarea name="wg" rows="7" placeholder="paste a wg-quick config:&#10;[Interface]&#10;PrivateKey = ...&#10;Address = 10.20.30.5/24&#10;[Peer]&#10;PublicKey = ...&#10;Endpoint = host:51820&#10;AllowedIPs = 0.0.0.0/0"></textarea>
  <button type="submit">save &amp; restart</button>
</form>
<p><small>after save: gr0m reboots and joins the network. on success the chest bolt flashes green for 2 seconds. paste a WireGuard config to also bring up a tunnel to your tailnet.</small></p>
</body></html>
)H";

static void handleRoot() {
  _portal.send(200, "text/html", HTML_FORM);
}

static void handleSave() {
  String ssid   = _portal.arg("ssid");
  String pwd    = _portal.arg("pwd");
  String bridge = _portal.arg("bridge");
  String wg     = _portal.arg("wg");

  Preferences p;
  p.begin("buddy", false);
  p.putString("wifi_ssid", ssid);
  p.putString("wifi_pwd",  pwd);
  p.putString("wifi_brg",  bridge);
  p.end();

  // Optional WireGuard config — parsed + stored under wg_* keys.
  // netWgInit() picks it up on the next boot after WiFi is online.
  if (wg.length() > 0) {
    netWgSaveConfigFromText(wg.c_str());
  }

  _portal.send(200, "text/html",
    "<html><body style='font:14px -apple-system,sans-serif;background:#0c0c0c;color:#e8e8e8;padding:40px;text-align:center'>"
    "<h1 style='color:#ff2c20'>saved</h1>"
    "<p style='color:#888'>gr0m is restarting&hellip;</p>"
    "</body></html>");
  delay(800);
  ESP.restart();
}

static void startPortal() {
  if (_portalRunning) {
    Serial.println("[wifi] portal already up — skipping init");
    return;
  }
  _state = NW_STARTING;
  Serial.printf("[wifi] startPortal() heap=%u\n", (unsigned)ESP.getFreeHeap());

  // BLE and WiFi share the single 2.4 GHz radio, and the BLE stack also
  // holds ~30-40 KB of heap. With BLE advertising active, softAP often
  // fails to allocate / get radio time (the "BLE coex?" failure). Pause
  // BLE advertising while the config portal is up — it's a one-time
  // setup step. BLE resumes on the next boot into STA mode, where
  // STA + BLE coexist fine.
  bleAdvertisingStop();
  delay(200);
  Serial.printf("[wifi] BLE adv paused for portal, heap=%u\n",
                (unsigned)ESP.getFreeHeap());

  // Retry softAP up to 5× — the radio scheduler usually settles within
  // a second or two of pausing BLE.
  bool ok = false;
  for (int attempt = 0; attempt < 5 && !ok; attempt++) {
    WiFi.mode(WIFI_AP);
    delay(200);
    ok = WiFi.softAP("gr0m-setup", "gr0mgr0m");
    Serial.printf("[wifi] softAP attempt %d: %s heap=%u\n", attempt + 1,
                  ok ? "OK" : "FAIL", (unsigned)ESP.getFreeHeap());
    if (!ok) delay(500);
  }
  if (!ok) {
    snprintf(_lastErr, sizeof(_lastErr), "softAP failed (BLE coex?)");
    _state = NW_FAILED;
    Serial.printf("[wifi] giving up — %s heap=%u\n", _lastErr,
                  (unsigned)ESP.getFreeHeap());
    return;
  }
  _portal.on("/", handleRoot);
  _portal.on("/save", HTTP_POST, handleSave);
  _portal.onNotFound([]() { _portal.send(404, "text/plain", "not found"); });
  _portal.begin();
  _portalRunning = true;
  _state = NW_PORTAL;
  _lastErr[0] = 0;
  snprintf(_ip, sizeof(_ip), "192.168.4.1");
  Serial.printf("[wifi] AP up — ssid=gr0m-setup pwd=gr0mgr0m url=192.168.4.1 heap=%u\n",
                (unsigned)ESP.getFreeHeap());
}

// STA connect attempt state — tracked async so we don't block the main
// loop while WiFi tries to associate.
static uint32_t _staStartedMs   = 0;
static bool     _staConnecting  = false;
static const uint32_t STA_TIMEOUT_MS = 12000;

void netWifiInit() {
  Serial.printf("[wifi] netWifiInit() setting=%s heap=%u\n",
                settings().wifi ? "on" : "off", (unsigned)ESP.getFreeHeap());
  if (!settings().wifi) {
    _state = NW_OFF;
    strncpy(_ip, "(off)", sizeof(_ip));
    return;
  }

  Preferences p;
  p.begin("buddy", true);
  String ssid = p.getString("wifi_ssid", "");
  String pwd  = p.getString("wifi_pwd",  "");
  String brg  = p.getString("wifi_brg",  "");
  p.end();
  strncpy(_bridge, brg.c_str(), sizeof(_bridge) - 1);
  _bridge[sizeof(_bridge) - 1] = 0;
  Serial.printf("[wifi] saved creds: ssid='%s' pwd_len=%d bridge='%s'\n",
                ssid.c_str(), (int)pwd.length(), _bridge);

  if (ssid.length() == 0) {
    Serial.println("[wifi] no saved SSID → opening config portal");
    startPortal();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("gr0m");
  // Keep WiFi up across drops without us doing the work.
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  // Event handler — drives reconnect feedback in the UI without
  // polling. Lambda captures nothing; modifies file-scope state.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case SYSTEM_EVENT_STA_DISCONNECTED:
        Serial.printf("[wifi] STA disconnected (reason=%d), auto-reconnecting\n",
                      info.wifi_sta_disconnected.reason);
        if (_state == NW_ONLINE) _state = NW_CONNECTING;
        snprintf(_ip, sizeof(_ip), "reconnecting...");
        break;
      case SYSTEM_EVENT_STA_GOT_IP:
        Serial.printf("[wifi] STA got IP: %s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        snprintf(_ip, sizeof(_ip), "%s", WiFi.localIP().toString().c_str());
        _state = NW_ONLINE;
        _staConnecting = false;
        _lastErr[0] = 0;
        // Kick off NTP so the ESP32 SYSTEM clock (time()) is valid. The
        // WireGuard handshake stamps a TAI64N timestamp read from the
        // system clock — without this it sits at 1970 and the tunnel
        // never comes up. UTC (offsets 0) is fine; WG doesn't care about
        // local time. netWgInit() is gated on time() being valid.
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("[wifi] NTP sync started (pool.ntp.org)");
        break;
      default: break;
    }
  });
  WiFi.begin(ssid.c_str(), pwd.c_str());
  Serial.printf("[wifi] STA begin: '%s' (async, %dms timeout)\n",
                ssid.c_str(), STA_TIMEOUT_MS);
  _staConnecting = true;
  _state         = NW_CONNECTING;
  _staStartedMs  = millis();
  _lastErr[0]    = 0;
  snprintf(_ip, sizeof(_ip), "connecting...");
}

void netWifiStop() {
  Serial.println("[wifi] netWifiStop()");
  _portal.stop();
  _portalRunning = false;
  _staConnecting = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  _state = NW_OFF;
  strncpy(_ip, "(off)", sizeof(_ip));
}

void netWifiTick() {
  if (_portalRunning) {
    _portal.handleClient();
    return;
  }
  if (_staConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      _staConnecting = false;
      _state = NW_ONLINE;
      snprintf(_ip, sizeof(_ip), "%s", WiFi.localIP().toString().c_str());
      Serial.printf("[wifi] connected: ip=%s rssi=%d\n", _ip, WiFi.RSSI());
    } else if (millis() - _staStartedMs > STA_TIMEOUT_MS) {
      _staConnecting = false;
      snprintf(_lastErr, sizeof(_lastErr), "STA timeout");
      Serial.println("[wifi] STA timeout — opening config portal");
      WiFi.disconnect(true);
      delay(50);
      startPortal();
    }
  }
}

NetWifiState netWifiState() { return _state; }
bool netWifiOnline()        { return _state == NW_ONLINE; }
bool netWifiPortalActive()  { return _portalRunning; }
int  netWifiRSSI()          { return WiFi.RSSI(); }
const char* netWifiIP()     { return _ip; }
const char* netWifiBridgeAddr() { return _bridge; }
const char* netWifiLastError() { return _lastErr; }
