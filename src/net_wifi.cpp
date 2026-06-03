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
  // holds ~30-40 KB of heap. With BLE active, softAP often fails to
  // allocate / get radio time (the "BLE coex?" failure). The radio is
  // WiFi XOR BLE: fully suspend BLE while the AP portal is up. Unlike a
  // plain advertising-stop, bleSetSuspended() also drops any live
  // connection and ignores incoming RX, so a still-paired peer can't keep
  // injecting commands while the AP owns the radio.
  bleSetSuspended(true);
  delay(200);
  Serial.printf("[wifi] BLE suspended for portal, heap=%u\n",
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

// ── Multi-network (round-robin) ─────────────────────────────────────
// Several saved networks, tried in turn. Stored in NVS as wifi_ssid0..,
// wifi_pwd0.. with a wifi_n count (the "wifi" BLE command writes these).
// We rotate on STA_TIMEOUT rather than scan-and-pick (WiFiMulti) so the
// main loop never blocks on a scan — the pet keeps animating. The
// GOT_IP event handler (registered once below) fires regardless of which
// network associates, so NTP + state transitions are network-agnostic.
static const int MAX_NETS = 4;
static char _ssids[MAX_NETS][33];
static char _pwds[MAX_NETS][65];
static int  _netCount = 0;
static int  _curNet   = 0;

static void _beginNet(int idx) {
  Serial.printf("[wifi] trying net %d/%d: '%s'\n", idx + 1, _netCount, _ssids[idx]);
  WiFi.begin(_ssids[idx], _pwds[idx]);
  _staConnecting = true;
  _state         = NW_CONNECTING;
  _staStartedMs  = millis();
  _lastErr[0]    = 0;
  snprintf(_ip, sizeof(_ip), "connecting...");
}

void netWifiInit() {
  // Read the enable flag straight from NVS. stats.h keeps settings() in a
  // file-static struct, so THIS translation unit has its own copy that
  // main.cpp's settingsLoad() never populates — reading settings().wifi
  // here always saw the default (false), which is why WiFi never came up.
  // NVS is the one source of truth shared across translation units.
  Preferences p;
  p.begin("buddy", true);
  bool enabled = p.getUChar("s_wifi", 0) != 0;
  Serial.printf("[wifi] netWifiInit() s_wifi=%d heap=%u\n",
                (int)enabled, (unsigned)ESP.getFreeHeap());
  if (!enabled) {
    p.end();
    _state = NW_OFF;
    strncpy(_ip, "(off)", sizeof(_ip));
    return;
  }

  // Radio mutex: WiFi is enabled, so it owns the single 2.4 GHz radio for
  // this boot. Suspend BLE up front (drops RX + advertising + any live link)
  // so no command can arrive over BLE while WiFi is active — true WiFi XOR
  // BLE. Serial is a separate UART and stays live. In practice BLE was never
  // init'd in WiFi mode (setup() gates startBt() on !wifi), but suspending
  // here is the belt-and-braces guarantee regardless of init order, and it
  // also covers the STA→portal fallback path inside this same boot.
  bleSetSuspended(true);

  // Load the saved network list (wifi_ssid0.., wifi_pwd0.., count wifi_n).
  // Fall back to the legacy single-slot keys (wifi_ssid/wifi_pwd) so a
  // device configured by the old portal still works.
  String brg = p.getString("wifi_brg", "");
  _netCount  = 0;
  uint8_t n  = p.getUChar("wifi_n", 0);
  for (uint8_t i = 0; i < n && _netCount < MAX_NETS; i++) {
    char ks[14], kp[14];
    snprintf(ks, sizeof(ks), "wifi_ssid%u", i);
    snprintf(kp, sizeof(kp), "wifi_pwd%u",  i);
    String s = p.getString(ks, "");
    if (s.length() == 0) continue;
    strncpy(_ssids[_netCount], s.c_str(), 32); _ssids[_netCount][32] = 0;
    String pw = p.getString(kp, "");
    strncpy(_pwds[_netCount], pw.c_str(), 64); _pwds[_netCount][64] = 0;
    _netCount++;
  }
  if (_netCount == 0) {
    String s = p.getString("wifi_ssid", "");
    if (s.length()) {
      strncpy(_ssids[0], s.c_str(), 32); _ssids[0][32] = 0;
      String pw = p.getString("wifi_pwd", "");
      strncpy(_pwds[0], pw.c_str(), 64); _pwds[0][64] = 0;
      _netCount = 1;
    }
  }
  p.end();
  strncpy(_bridge, brg.c_str(), sizeof(_bridge) - 1);
  _bridge[sizeof(_bridge) - 1] = 0;
  Serial.printf("[wifi] %d saved network(s), bridge='%s'\n", _netCount, _bridge);

  if (_netCount == 0) {
    Serial.println("[wifi] no saved networks → opening config portal");
    startPortal();
    return;
  }

  WiFi.mode(WIFI_STA);
  // Battery saver: modem-sleep between the AP's DTIM beacons. The association
  // and the WireGuard tunnel stay up, but the radio powers down between beacon
  // intervals — the single biggest WiFi power win on battery. Worst-case prompt
  // latency rises by one beacon interval (~100ms), which the approval path
  // already tolerates. (Usually the arduino-esp32 default; set explicitly here.)
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.setHostname("gr0m");
  // Round-robin owns the INITIAL connect — auto-reconnect must stay off
  // here or it issues a competing WiFi.begin() to the previous SSID while
  // we rotate, which (with a disconnect mid-flight) deinits the driver and
  // crashes. We turn auto-reconnect ON only once we have an IP (GOT_IP),
  // handing off "stay connected" duty to the stack.
  WiFi.setAutoReconnect(false);
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
        Serial.printf("[wifi] STA got IP: %s\n",
                      WiFi.localIP().toString().c_str());
        snprintf(_ip, sizeof(_ip), "%s", WiFi.localIP().toString().c_str());
        _state = NW_ONLINE;
        _staConnecting = false;
        _lastErr[0] = 0;
        // Connected — now let the stack keep us connected across drops.
        WiFi.setAutoReconnect(true);
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
  _curNet = 0;
  _beginNet(_curNet);
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
  // Radio mutex: WiFi released the radio. Resume BLE so it can advertise and
  // receive again. (In practice the radio switch goes through a reboot via
  // applyConn(), but releasing the suspend here keeps the invariant honest if
  // WiFi is ever stopped without one.)
  bleSetSuspended(false);
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
      Serial.printf("[wifi] connected: ip=%s\n", _ip);
    } else if (millis() - _staStartedMs > STA_TIMEOUT_MS) {
      // status tells us WHY: 1=WL_NO_SSID_AVAIL (not visible / 5GHz-only),
      // 4=WL_CONNECT_FAILED (bad password), 6=WL_DISCONNECTED (general).
      Serial.printf("[wifi] '%s' failed, status=%d\n",
                    _ssids[_curNet], (int)WiFi.status());
      WiFi.disconnect(false);   // leave driver up — begin() switches AP cleanly
      delay(50);
      if (_netCount > 1) {
        // Rotate to the next saved network and keep trying. A travelling
        // pet is rarely in range of every network at once, so we cycle
        // indefinitely rather than give up — reconfig is over BLE anyway.
        _curNet = (_curNet + 1) % _netCount;
        Serial.printf("[wifi] STA timeout — rotating to net %d\n", _curNet);
        _beginNet(_curNet);
      } else {
        _staConnecting = false;
        snprintf(_lastErr, sizeof(_lastErr), "STA timeout");
        Serial.println("[wifi] STA timeout — opening config portal");
        startPortal();
      }
    }
  }
}

NetWifiState netWifiState() { return _state; }
bool netWifiOnline()        { return _state == NW_ONLINE; }
bool netWifiPortalActive()  { return _portalRunning; }
const char* netWifiIP()     { return _ip; }
const char* netWifiBridgeAddr() { return _bridge; }
const char* netWifiLastError() { return _lastErr; }
// Live count of stations associated to the setup AP. Read at draw time by the
// CONNECTIONS screen so a phone joining the portal shows up immediately. 0
// when the AP isn't running.
int netWifiApClients() { return _portalRunning ? (int)WiFi.softAPgetStationNum() : 0; }
