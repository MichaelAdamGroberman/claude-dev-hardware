#include "net_tcp.h"
#include "net_wifi.h"
#include "ble_bridge.h"   // bleInjectRx() — feeds the shared command pipeline
#include <WiFi.h>
#include <Preferences.h>
#include <string.h>

static const uint16_t TCP_PORT = 6400;

// ── Inbound listener (device = server) — used on the LAN ────────────
static WiFiServer _server(TCP_PORT);
static WiFiClient _client;
static bool   _started = false;
static bool   _authed  = false;
static char   _token[48] = "";
// Match the firmware's main line buffer (data.h _LineBuf<1024>) so large
// commands like a full multi-network `wifi` provision (~420 B) aren't
// truncated and dropped here before reaching the parser.
static char   _line[1024];
static size_t _lineLen = 0;

// ── Outbound dial (device = client) — used for VPN / off-LAN ────────
// Inbound to the tunnel IP isn't delivered by the WG lib, so for remote
// reach the device instead DIALS OUT to a bridge that listens. Same
// protocol, same token; reuses the outbound path the WG handshake proved.
static WiFiClient _out;
static char     _peerHost[64] = "";
static uint16_t _peerPort     = 0;
static bool     _outAuthed    = false;
static uint32_t _outNextTry   = 0;
static char     _outLine[1024];
static size_t   _outLen       = 0;

void netTcpInit() {
  Preferences p;
  p.begin("buddy", true);
  String t  = p.getString("tcp_token", "");
  String pr = p.getString("tcp_peer",  "");   // "host:port" → enable dial-out
  p.end();
  strncpy(_token, t.c_str(), sizeof(_token) - 1); _token[sizeof(_token) - 1] = 0;

  // Parse optional dial-out peer.
  if (pr.length()) {
    int c = pr.lastIndexOf(':');
    if (c > 0) {
      String h = pr.substring(0, c);
      strncpy(_peerHost, h.c_str(), sizeof(_peerHost) - 1);
      _peerHost[sizeof(_peerHost) - 1] = 0;
      _peerPort = (uint16_t)pr.substring(c + 1).toInt();
    }
  }

  if (_token[0] == 0) {
    Serial.println("[tcp] no token provisioned — network bridge disabled");
    return;
  }
  _server.begin();
  _server.setNoDelay(true);
  _started = true;
  Serial.printf("[tcp] listening on :%u (token required)\n", TCP_PORT);
  if (_peerHost[0])
    Serial.printf("[tcp] dial-out peer %s:%u\n", _peerHost, _peerPort);
}

// Authenticated lines from EITHER transport flow into the same RX path the
// BLE link uses, so commands run through _applyJson()/xferCommand() intact.
static void _inject(char* line) {
  bleInjectRx((const uint8_t*)line, strlen(line));
  bleInjectRx((const uint8_t*)"\n", 1);
}

// ── Inbound listener pump ───────────────────────────────────────────
static void _serverTick() {
  if (!_client || !_client.connected()) {
    WiFiClient c = _server.available();
    if (c) {
      _client = c; _authed = false; _lineLen = 0;
      Serial.println("[tcp] client connected — awaiting token");
    }
    return;
  }
  while (_client.available()) {
    char ch = (char)_client.read();
    if (ch == '\n' || ch == '\r') {
      if (_lineLen == 0) continue;
      _line[_lineLen] = 0;
      if (!_authed) {
        if (_token[0] && strcmp(_line, _token) == 0) {
          _authed = true;
          const char* ok = "{\"auth\":\"ok\"}\n";
          _client.write((const uint8_t*)ok, strlen(ok));
          Serial.println("[tcp] client authenticated");
        } else {
          _client.write((const uint8_t*)"{\"auth\":\"fail\"}\n", 16);
          _client.stop();
          Serial.println("[tcp] auth failed — dropped");
        }
      } else {
        _inject(_line);
      }
      _lineLen = 0;
    } else if (_lineLen < sizeof(_line) - 1) {
      _line[_lineLen++] = ch;
    }
  }
}

// ── Outbound dial pump ──────────────────────────────────────────────
static void _outTick() {
  if (_peerHost[0] == 0) return;
  if (!_out.connected()) {
    _outAuthed = false;
    uint32_t now = millis();
    if (now < _outNextTry) return;
    _outNextTry = now + 20000;         // retry every 20s while down
    // Short connect timeout: connect() is blocking, so a dead/unreachable
    // peer (e.g. tunnel down) must not stall the render loop for seconds.
    if (_out.connect(_peerHost, _peerPort, 1200)) {
      _out.setNoDelay(true);
      _out.printf("%s\n", _token);     // authenticate ourselves to the bridge
      _outAuthed = true;
      _outLen = 0;
      Serial.printf("[tcp] dialed out to %s:%u\n", _peerHost, _peerPort);
    }
    return;
  }
  while (_out.available()) {
    char ch = (char)_out.read();
    if (ch == '\n' || ch == '\r') {
      if (_outLen > 0) { _outLine[_outLen] = 0; _inject(_outLine); _outLen = 0; }
    } else if (_outLen < sizeof(_outLine) - 1) {
      _outLine[_outLen++] = ch;
    }
  }
}

void netTcpTick() {
  if (!_started) return;
  _serverTick();
  _outTick();
}

bool netTcpClientConnected() {
  bool srv = _started && _client && _client.connected() && _authed;
  bool out = _started && _out.connected() && _outAuthed;
  return srv || out;
}

size_t netTcpWrite(const uint8_t* data, size_t len) {
  if (!_started) return 0;
  size_t n = 0;
  // Mirror to whichever transport(s) are attached.
  if (_client && _client.connected() && _authed) n = _client.write(data, len);
  if (_out.connected() && _outAuthed)            _out.write(data, len);
  return n;
}
