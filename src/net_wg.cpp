#include "net_wg.h"
#include "net_wifi.h"
#include <WireGuard-ESP32.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>

static WireGuard _wg;
static NetWgState _state = WG_OFF;
static char _tunIp[24]   = "(off)";
static char _endpoint[64] = "";
static char _lastErr[64] = "";

// NVS-cached config; loaded once in netWgInit(), kept here so getters
// don't need to re-open Preferences every frame.
static char _ifAddr[24]    = "";  // "10.20.30.5"
static char _ifPriv[48]    = "";  // base64 private key
static char _peerPub[48]   = "";  // base64 peer public key
static char _peerHost[48]  = "";  // host or IP
static uint16_t _peerPort  = 51820;
// Handshake-completion poll cadence — the library doesn't expose an
// event callback, so we re-check WireGuard.is_initialized() periodically.
static uint32_t _nextPollMs = 0;
static const uint32_t POLL_INTERVAL_MS = 1500;

static void loadNvs() {
  Preferences p;
  p.begin("buddy", true);
  String ifAddr  = p.getString("wg_if_addr",  "");
  String ifPriv  = p.getString("wg_if_priv",  "");
  String peerPub = p.getString("wg_peer_pub", "");
  String peerEp  = p.getString("wg_peer_ep",  "");
  uint16_t peerP = p.getUShort("wg_peer_port", 51820);
  p.end();

  strncpy(_ifAddr,   ifAddr.c_str(),  sizeof(_ifAddr) - 1);
  strncpy(_ifPriv,   ifPriv.c_str(),  sizeof(_ifPriv) - 1);
  strncpy(_peerPub,  peerPub.c_str(), sizeof(_peerPub) - 1);
  strncpy(_peerHost, peerEp.c_str(),  sizeof(_peerHost) - 1);
  _peerPort = peerP;
  _ifAddr[sizeof(_ifAddr) - 1]    = 0;
  _ifPriv[sizeof(_ifPriv) - 1]    = 0;
  _peerPub[sizeof(_peerPub) - 1]  = 0;
  _peerHost[sizeof(_peerHost) - 1] = 0;
}

void netWgInit() {
  loadNvs();
  if (_ifAddr[0] == 0 || _ifPriv[0] == 0 || _peerPub[0] == 0 || _peerHost[0] == 0) {
    _state = WG_OFF;
    strncpy(_tunIp, "(off)", sizeof(_tunIp));
    return;
  }
  // Render endpoint string for status getters.
  snprintf(_endpoint, sizeof(_endpoint), "%s:%u", _peerHost, _peerPort);

  // WireGuard.begin signature from ciniml/WireGuard-ESP32:
  //   bool begin(const IPAddress& localIP, const char* privateKey,
  //              const char* remotePeerAddress, const char* remotePeerPublicKey,
  //              uint16_t remotePeerPort);
  IPAddress local;
  local.fromString(_ifAddr);
  bool ok = _wg.begin(local, _ifPriv, _peerHost, _peerPub, _peerPort);
  if (!ok) {
    snprintf(_lastErr, sizeof(_lastErr), "WireGuard.begin returned false");
    _state = WG_FAILED;
    return;
  }
  _state = WG_STARTING;
  strncpy(_tunIp, _ifAddr, sizeof(_tunIp));
  _lastErr[0] = 0;
  _nextPollMs = millis() + POLL_INTERVAL_MS;
}

void netWgStop() {
  _wg.end();
  _state = WG_OFF;
  strncpy(_tunIp, "(off)", sizeof(_tunIp));
}

void netWgTick() {
  if (_state == WG_OFF || _state == WG_FAILED) return;
  uint32_t now = millis();
  if (now < _nextPollMs) return;
  _nextPollMs = now + POLL_INTERVAL_MS;
  // is_initialized() returns true after the first successful handshake.
  // Before that the library is bound but no peer rekey has happened yet.
  if (_wg.is_initialized()) {
    _state = WG_UP;
  }
}

NetWgState  netWgState()        { return _state; }
const char* netWgTunnelIP()     { return _tunIp; }
const char* netWgPeerEndpoint() { return _endpoint; }
const char* netWgLastError()    { return _lastErr; }

// ── wg-quick config parser ──────────────────────────────────────────
// Returns true if we saw at least PrivateKey, Address, PublicKey, and
// Endpoint. AllowedIPs is read but not used yet (this library always
// routes everything through the tunnel — no per-route splitting).

static const char* skipWs(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static bool getKV(const char* line, const char* key, char* out, size_t outsz) {
  size_t klen = strlen(key);
  const char* s = skipWs(line);
  if (strncasecmp(s, key, klen) != 0) return false;
  s += klen;
  s = skipWs(s);
  if (*s != '=') return false;
  s++;
  s = skipWs(s);
  size_t n = 0;
  while (*s && *s != '\r' && *s != '\n' && n < outsz - 1) {
    out[n++] = *s++;
  }
  // Trim trailing whitespace.
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
  out[n] = 0;
  return n > 0;
}

bool netWgSaveConfigFromText(const char* text) {
  char ifPriv[48] = "";
  char ifAddr[24] = "";
  char peerPub[48] = "";
  char peerEp[64] = "";

  // Walk line by line.
  const char* p = text;
  char line[160];
  while (*p) {
    size_t n = 0;
    while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
    line[n] = 0;
    if (*p == '\n') p++;
    if (line[0] == '[' || line[0] == '#' || line[0] == 0) continue;
    char val[160];
    if (getKV(line, "PrivateKey", val, sizeof(val))) {
      strncpy(ifPriv, val, sizeof(ifPriv) - 1); ifPriv[sizeof(ifPriv) - 1] = 0;
    } else if (getKV(line, "Address", val, sizeof(val))) {
      // Strip any "/cidr" suffix — the lib wants a bare IP.
      char* slash = strchr(val, '/');
      if (slash) *slash = 0;
      strncpy(ifAddr, val, sizeof(ifAddr) - 1); ifAddr[sizeof(ifAddr) - 1] = 0;
    } else if (getKV(line, "PublicKey", val, sizeof(val))) {
      strncpy(peerPub, val, sizeof(peerPub) - 1); peerPub[sizeof(peerPub) - 1] = 0;
    } else if (getKV(line, "Endpoint", val, sizeof(val))) {
      strncpy(peerEp, val, sizeof(peerEp) - 1); peerEp[sizeof(peerEp) - 1] = 0;
    }
  }

  if (!ifPriv[0] || !ifAddr[0] || !peerPub[0] || !peerEp[0]) return false;

  // Split host:port.
  char host[48] = ""; uint16_t port = 51820;
  const char* colon = strrchr(peerEp, ':');
  if (colon) {
    size_t hl = colon - peerEp;
    if (hl >= sizeof(host)) hl = sizeof(host) - 1;
    memcpy(host, peerEp, hl); host[hl] = 0;
    port = (uint16_t)atoi(colon + 1);
  } else {
    strncpy(host, peerEp, sizeof(host) - 1); host[sizeof(host) - 1] = 0;
  }
  if (port == 0) port = 51820;

  Preferences pf;
  pf.begin("buddy", false);
  pf.putString("wg_if_addr",  ifAddr);
  pf.putString("wg_if_priv",  ifPriv);
  pf.putString("wg_peer_pub", peerPub);
  pf.putString("wg_peer_ep",  host);
  pf.putUShort("wg_peer_port", port);
  pf.end();
  return true;
}
