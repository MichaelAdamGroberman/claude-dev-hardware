#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "ble_bridge.h"
#include <mbedtls/base64.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "net_wg.h"

static File     _xFile;
static uint32_t _xExpected = 0, _xWritten = 0;
static char     _xCharName[24] = "";
static bool     _xActive = false;
static bool     _xFileOpen = false;
static uint32_t _xTotal = 0, _xTotalWritten = 0;
static const char* _X_CHARS_DIR = "/characters";
static const char* _X_STAGE_DIR = "/.char_stage";

void characterClose();
bool characterInit(const char* name);

// Ack goes to both streams — we don't track which one delivered the command,
// and writes to a clientless SerialBT just drop. The bridge listens on
// whichever port it opened.
static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok?"true":"false", (unsigned long)n);
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}

static const char* _xBaseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool _xSafePathComponent(const char* s) {
  if (!s || !*s || strcmp(s, ".") == 0 || strstr(s, "..")) return false;
  for (const char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == '/' || c == '\\') return false;
  }
  return true;
}

static void _xCloseFile() {
  if (_xFileOpen) {
    _xFile.close();
    _xFileOpen = false;
  }
}

static uint32_t _xWipeDir(const char* dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) { LittleFS.mkdir(dir); return 0; }
  uint32_t freed = 0;
  File f = d.openNextFile();
  while (f) {
    freed += f.size();
    char p[80];
    snprintf(p, sizeof(p), "%s/%s", dir, _xBaseName(f.name()));
    f.close();
    LittleFS.remove(p);
    f = d.openNextFile();
  }
  d.close();
  return freed;
}

// Only one character lives on the device at a time. Installing a new one
// under a different name would otherwise leave the old one's files eating
// space. Wipe everything under /characters/, return total bytes reclaimed.
static uint32_t _xWipeAllChars() {
  File root = LittleFS.open(_X_CHARS_DIR);
  if (!root || !root.isDirectory()) { LittleFS.mkdir(_X_CHARS_DIR); return 0; }
  uint32_t freed = 0;
  File sub = root.openNextFile();
  while (sub) {
    const char* name = _xBaseName(sub.name());
    if (sub.isDirectory()) {
      char p[64];
      snprintf(p, sizeof(p), "%s/%s", _X_CHARS_DIR, name);
      sub.close();
      freed += _xWipeDir(p);
      LittleFS.rmdir(p);
    } else {
      char p[64];
      snprintf(p, sizeof(p), "%s/%s", _X_CHARS_DIR, name);
      freed += sub.size();
      sub.close();
      LittleFS.remove(p);
    }
    sub = root.openNextFile();
  }
  root.close();
  return freed;
}

static void _xCleanupStage() {
  _xWipeDir(_X_STAGE_DIR);
  LittleFS.rmdir(_X_STAGE_DIR);
}

static void _xAbortTransfer() {
  _xCloseFile();
  _xActive = false;
  _xCharName[0] = 0;
  _xExpected = _xWritten = 0;
  _xTotal = _xTotalWritten = 0;
  _xCleanupStage();
}

static bool _xPrepareStage() {
  _xCleanupStage();
  return LittleFS.mkdir(_X_STAGE_DIR);
}

static bool _xValidateStagedManifest() {
  char mpath[64];
  snprintf(mpath, sizeof(mpath), "%s/manifest.json", _X_STAGE_DIR);
  File mf = LittleFS.open(mpath, "r");
  if (!mf) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, mf);
  mf.close();
  if (err) return false;
  JsonObject states = doc["states"];
  return !states.isNull();
}

static bool _xCommitStagedChar() {
  char finalDir[64];
  snprintf(finalDir, sizeof(finalDir), "%s/%s", _X_CHARS_DIR, _xCharName);

  File d = LittleFS.open(_X_STAGE_DIR);
  if (!d || !d.isDirectory()) return false;
  char names[40][32];
  uint8_t n = 0;
  File f = d.openNextFile();
  while (f) {
    if (n >= 40) { f.close(); d.close(); return false; }
    const char* bn = _xBaseName(f.name());
    strncpy(names[n], bn, sizeof(names[n]) - 1);
    names[n][sizeof(names[n]) - 1] = 0;
    n++;
    f.close();
    f = d.openNextFile();
  }
  d.close();

  characterClose();
  _xWipeAllChars();
  LittleFS.mkdir(_X_CHARS_DIR);
  if (!LittleFS.mkdir(finalDir)) return false;

  for (uint8_t i = 0; i < n; i++) {
    char src[80], dst[96];
    snprintf(src, sizeof(src), "%s/%s", _X_STAGE_DIR, names[i]);
    snprintf(dst, sizeof(dst), "%s/%s", finalDir, names[i]);
    if (!LittleFS.rename(src, dst)) return false;
  }
  LittleFS.rmdir(_X_STAGE_DIR);
  return true;
}

// Called from data.h when incoming JSON has a "cmd" key. Returns true if
// it was a transfer command (caller should skip state-update parsing).
// Needs characterClose()/characterInit() declared before this include.
void characterClose();
bool characterInit(const char* name);
void petNameSet(const char* name);
const char* petName();
void ownerSet(const char* name);
const char* ownerName();
#include "stats.h"
#include <M5StickCPlus.h>

inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "species") == 0) {
    extern bool buddyMode, gifAvailable;
    extern void buddySetSpeciesIdx(uint8_t);
    uint8_t idx = doc["idx"] | 0xFF;
    speciesIdxSave(idx);
    buddyMode = !(gifAvailable && idx == 0xFF);
    if (buddyMode) buddySetSpeciesIdx(idx);
    _xAck("species", true);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  // Provision WiFi + WireGuard over BLE, bypassing the softAP portal
  // entirely (AP+BLE coexistence is unreliable on this chip; STA+BLE is
  // fine). Shape:
  //   {"cmd":"wifi","nets":[{"ssid":"..","pwd":".."},..],
  //    "wg":"<wg-quick text>","apply":true}
  // Writes the network list to NVS (wifi_ssidN/wifi_pwdN + wifi_n), the
  // WG config via the existing parser, flips the wifi setting on, and
  // (if apply) reboots straight into STA → WireGuard.
  if (strcmp(cmd, "wifi") == 0) {
    Preferences p;
    p.begin("buddy", false);
    uint8_t i = 0;
    JsonArray nets = doc["nets"];
    if (!nets.isNull()) {
      for (JsonVariant v : nets) {
        const char* s  = v["ssid"];
        const char* pw = v["pwd"] | "";
        if (s && *s && i < 4) {
          char ks[14], kp[14];
          snprintf(ks, sizeof(ks), "wifi_ssid%u", i);
          snprintf(kp, sizeof(kp), "wifi_pwd%u",  i);
          p.putString(ks, s);
          p.putString(kp, pw);
          i++;
        }
      }
    }
    p.putUChar("wifi_n", i);
    // Optional bridge token — enables the TCP listener (WiFi/WireGuard
    // transport). Without it the network listener stays off.
    const char* tok = doc["token"];
    if (tok && *tok) p.putString("tcp_token", tok);
    // Optional dial-out peer "host:port" — device connects OUT to a
    // listening bridge (used for VPN, where inbound to the device fails).
    const char* peer = doc["peer"];
    if (peer && *peer) p.putString("tcp_peer", peer);
    // Write the enable flag in the SAME transaction as the networks.
    // settingsSave() reuses the shared global Preferences object; routing
    // s_wifi through this fresh local handle avoids the case where that
    // shared handle is already-started and silently no-ops the write.
    if (i > 0) {
      p.putBool("s_wifi", true);
      settings().wifi = true;   // keep in-memory state consistent
    }
    p.end();

    const char* wg = doc["wg"];
    if (wg && *wg) netWgSaveConfigFromText(wg);

    // Diagnostic read-back: prove what actually landed in NVS.
    {
      Preferences q;
      q.begin("buddy", true);
      Serial.printf("[wifi] provisioned: s_wifi=%d wifi_n=%u ssid0='%s'\n",
                    (int)q.getBool("s_wifi", false), q.getUChar("wifi_n", 0),
                    q.getString("wifi_ssid0", "").c_str());
      q.end();
    }

    _xAck("wifi", i > 0, i);

    if ((doc["apply"] | false) && i > 0) {
      delay(400);          // let the ack flush over BLE before we drop the link
      ESP.restart();
    }
    return true;
  }

  // Usage push from the bridge daemon. set=period figure (chest-LCD),
  // life=lifetime (drives level + evolution stage), ok/deny=approvals/denials
  // this period. Missing fields keep their current value (back-compatible with
  // an older daemon that only sends "set").
  //   {"cmd":"tokens","set":N,"life":N,"ok":N,"deny":N}
  if (strcmp(cmd, "tokens") == 0) {
    uint32_t period = doc["set"]  | stats().tokens;
    uint32_t life   = doc["life"] | stats().lifetimeTokens;
    uint16_t ok     = (uint16_t)(doc["ok"]   | (int)stats().okCount);
    uint16_t no     = (uint16_t)(doc["deny"] | (int)stats().noCount);
    statsSetTokens(period, life, ok, no);
    _xAck("tokens", true);
    return true;
  }

  // Switch radio mode. WiFi and BLE are mutually exclusive (shared radio);
  // serial always works regardless. {"cmd":"radio","mode":"wifi"|"bt"|"off"}
  // writes the settings and reboots into the chosen mode.
  if (strcmp(cmd, "radio") == 0) {
    const char* mode = doc["mode"] | "off";
    bool wantWifi = (strcmp(mode, "wifi") == 0);
    bool wantBt   = (strcmp(mode, "bt")   == 0);
    Preferences p;
    p.begin("buddy", false);
    p.putBool("s_wifi", wantWifi);
    p.putBool("s_bt",   wantBt);
    p.end();
    settings().wifi = wantWifi;
    settings().bt   = wantBt;
    _xAck("radio", true);
    delay(400);
    ESP.restart();
    return true;
  }

  // Adapter mode — strip the desk-pet (skip animation + mic, quiet BLE
  // advertising) so the device is a focused GPIO / logic-analyzer probe.
  // In-memory only: a reset returns to normal BT/WiFi pet mode.
  //   {"cmd":"adapter","on":true|false}
  if (strcmp(cmd, "adapter") == 0) {
    extern bool adapterMode;
    bool on = (doc["on"] | true);
    adapterMode = on;
    // Only quiet BLE if it isn't the active command link — if a BLE client
    // is connected, leave advertising up so the channel (and reconnects)
    // keep working. Serial/WiFi links are unaffected either way.
    if (on) { if (!bleConnected()) bleAdvertisingStop(); }
    else bleAdvertisingStart();
    _xAck("adapter", on);
    return true;
  }

  // Dismiss the approval prompt currently shown on the device. The bridge
  // sends this when the prompt was already resolved on the computer (or
  // expired), so the stick shouldn't keep showing a stale APPROVE screen.
  // The prompt state (tama / lastPromptId / responseSent) all lives in
  // main.cpp; clearPromptState() there clears it and returns to the normal
  // screen — declared extern here the same way `adapter` reaches adapterMode.
  //   {"cmd":"clearprompt"}
  if (strcmp(cmd, "clearprompt") == 0) {
    extern void clearPromptState();
    clearPromptState();
    _xAck("clearprompt", true);
    return true;
  }

  // GPIO / mini logic-analyzer on the exposed header pins. Restricted to
  // pins that aren't wired to the display/IMU/buttons/IR/mic so we can't
  // brick the device. Replies with {"ack":"gpio",...} carrying the result.
  //   {"cmd":"gpio","act":"read"|"write"|"mode"|"adc"|"cap","pin":N,...}
  //   write: "value":0|1   mode: "mode":"input"|"output"|"pullup"
  //   adc:   analog read    cap: "n":<=512 samples, "us":interval -> "bits" hex
  if (strcmp(cmd, "gpio") == 0) {
    static const int SAFE[] = {0, 25, 26, 32, 33, 36};
    const char* act = doc["act"] | "read";
    int pin = doc["pin"] | -1;
    bool safe = false;
    for (unsigned k = 0; k < sizeof(SAFE)/sizeof(SAFE[0]); k++) if (SAFE[k] == pin) safe = true;
    char b[640];
    int len;
    if (!safe) {
      len = snprintf(b, sizeof(b),
        "{\"ack\":\"gpio\",\"ok\":false,\"pin\":%d,\"error\":\"pin not allowed; use 0,25,26,32,33,36\"}\n", pin);
    } else if (strcmp(act, "mode") == 0) {
      const char* m = doc["mode"] | "input";
      pinMode(pin, strcmp(m,"output")==0 ? OUTPUT : strcmp(m,"pullup")==0 ? INPUT_PULLUP : INPUT);
      len = snprintf(b, sizeof(b), "{\"ack\":\"gpio\",\"ok\":true,\"act\":\"mode\",\"pin\":%d,\"mode\":\"%s\"}\n", pin, m);
    } else if (strcmp(act, "write") == 0) {
      int v = (doc["value"] | 0) ? 1 : 0;
      pinMode(pin, OUTPUT); digitalWrite(pin, v);
      len = snprintf(b, sizeof(b), "{\"ack\":\"gpio\",\"ok\":true,\"act\":\"write\",\"pin\":%d,\"value\":%d}\n", pin, v);
    } else if (strcmp(act, "adc") == 0) {
      int v = analogRead(pin);
      len = snprintf(b, sizeof(b), "{\"ack\":\"gpio\",\"ok\":true,\"act\":\"adc\",\"pin\":%d,\"raw\":%d,\"mv\":%d}\n",
                     pin, v, (int)((long)v * 3300 / 4095));
    } else if (strcmp(act, "cap") == 0) {
      int n = doc["n"] | 128; if (n > 512) n = 512; if (n < 1) n = 1;
      int iv = doc["us"] | 50; if (iv < 2) iv = 2;
      if ((long)n * iv > 400000) n = 400000 / iv;   // cap the blocking window ~0.4s
      pinMode(pin, INPUT);
      uint8_t bits[64]; memset(bits, 0, sizeof(bits));
      uint32_t t0 = micros();
      for (int i = 0; i < n; i++) {
        uint32_t due = t0 + (uint32_t)i * iv;
        while ((int32_t)(micros() - due) < 0) {}
        if (digitalRead(pin)) bits[i >> 3] |= (1 << (i & 7));
      }
      uint32_t el = micros() - t0;
      int nb = (n + 7) / 8; char hex[130];
      for (int i = 0; i < nb; i++) snprintf(hex + i*2, 3, "%02x", bits[i]);
      len = snprintf(b, sizeof(b),
        "{\"ack\":\"gpio\",\"ok\":true,\"act\":\"cap\",\"pin\":%d,\"n\":%d,\"us\":%d,\"elapsed_us\":%lu,\"bits\":\"%s\"}\n",
        pin, n, iv, (unsigned long)el, hex);
    } else {
      pinMode(pin, INPUT); int v = digitalRead(pin);
      len = snprintf(b, sizeof(b), "{\"ack\":\"gpio\",\"ok\":true,\"act\":\"read\",\"pin\":%d,\"value\":%d}\n", pin, v);
    }
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // Dump everything the info screens show. Manual printf rather than
    // ArduinoJson serialize — less heap churn, and the shape is fixed.
    int vBat = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat = (int)M5.Axp.GetBatCurrent();
    int vBus = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat - 3200) / 10;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    char b[320];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":%d,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%u,\"fsFree\":%lu,\"fsTotal\":%lu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      pct, vBat, iBat, (vBus > 4000) ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap(),
      (unsigned long)(LittleFS.totalBytes() - LittleFS.usedBytes()),
      (unsigned long)LittleFS.totalBytes(),
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  if (strcmp(cmd, "char_begin") == 0) {
    const char* name = doc["name"] | "pet";
    uint32_t newTotal = doc["total"] | 0;
    if (_xActive) _xAbortTransfer();
    _xTotal = newTotal;

    if (!_xSafePathComponent(name) || strlen(name) >= sizeof(_xCharName)) {
      _xAck("char_begin", false);
      return true;
    }

    // Safe staging: the active character remains in /characters while the new
    // pack lands in _X_STAGE_DIR. If there isn't enough current free space for
    // the staged pack, reject without touching the installed character.
    uint32_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
    // Headroom for LittleFS metadata overhead — it's not byte-for-byte.
    if (_xTotal > 0 && _xTotal + 4096 > free) {
      char b[96];
      int len = snprintf(b, sizeof(b),
        "{\"ack\":\"char_begin\",\"ok\":false,\"n\":%lu,\"error\":\"need %luK, have %luK\"}\n",
        (unsigned long)free, (unsigned long)(_xTotal/1024), (unsigned long)(free/1024)
      );
      Serial.write(b, len);
      bleWrite((const uint8_t*)b, len);
      return true;
    }

    strncpy(_xCharName, name, sizeof(_xCharName)-1); _xCharName[sizeof(_xCharName)-1]=0;
    if (!_xPrepareStage()) {
      _xCharName[0] = 0;
      _xAck("char_begin", false);
      return true;
    }
    _xTotalWritten = 0;
    _xExpected = _xWritten = 0;
    _xActive = true;
    _xAck("char_begin", true);
    return true;
  }

  if (!_xActive) return strcmp(cmd, "permission") != 0;  // permission cmd is not ours

  if (strcmp(cmd, "file") == 0) {
    const char* path = doc["path"];
    _xExpected = doc["size"] | 0;
    _xWritten = 0;
    if (_xFileOpen || !_xSafePathComponent(path) ||
        (_xTotal > 0 && _xExpected > 0 && _xTotalWritten + _xExpected > _xTotal)) {
      _xAbortTransfer();
      _xAck("file", false);
      return true;
    }
    char full[80]; snprintf(full, sizeof(full), "%s/%s", _X_STAGE_DIR, path);
    _xFile = LittleFS.open(full, "w");
    _xFileOpen = (bool)_xFile;
    if (!_xFileOpen) _xAbortTransfer();
    _xAck("file", _xFileOpen);
    return true;
  }

  if (strcmp(cmd, "chunk") == 0) {
    const char* b64 = doc["d"];
    if (!b64 || !_xFileOpen) {
      uint32_t n = _xWritten;
      _xAbortTransfer();
      _xAck("chunk", false, n);
      return true;
    }
    uint8_t buf[300];
    size_t outLen = 0;
    int rc = mbedtls_base64_decode(buf, sizeof(buf), &outLen,
                                   (const uint8_t*)b64, strlen(b64));
    if (rc != 0 ||
        (_xExpected > 0 && _xWritten + outLen > _xExpected) ||
        (_xTotal > 0 && _xTotalWritten + outLen > _xTotal)) {
      uint32_t n = _xWritten;
      _xAbortTransfer();
      _xAck("chunk", false, n);
      return true;
    }
    size_t wr = _xFile.write(buf, outLen);
    if (wr != outLen) {
      uint32_t n = _xWritten;
      _xAbortTransfer();
      _xAck("chunk", false, n);
      return true;
    }
    _xWritten += outLen;
    _xTotalWritten += outLen;
    // Ack every chunk — LittleFS writes can block on flash erase and the
    // UART RX buffer is only ~256 bytes. Without this the sender overruns it.
    _xAck("chunk", true, _xWritten);
    return true;
  }

  if (strcmp(cmd, "file_end") == 0) {
    bool ok = _xFileOpen && (_xWritten == _xExpected || _xExpected == 0);
    uint32_t n = _xWritten;
    _xCloseFile();
    if (!ok) _xAbortTransfer();
    _xAck("file_end", ok, n);
    return true;
  }

  if (strcmp(cmd, "char_end") == 0) {
    bool ok = false;
    if (_xFileOpen) {
      _xAbortTransfer();
      _xAck("char_end", false);
      return true;
    }
    if (_xValidateStagedManifest() && _xCommitStagedChar()) {
      ok = characterInit(_xCharName);
    }
    _xActive = false;
    extern bool buddyMode, gifAvailable;
    if (ok) { buddyMode = false; gifAvailable = true; speciesIdxSave(0xFF); }
    else _xCleanupStage();
    _xCharName[0] = 0;
    _xExpected = _xWritten = 0;
    _xTotal = _xTotalWritten = 0;
    _xAck("char_end", ok);
    return true;
  }

  return false;
}

inline bool xferActive() { return _xActive; }
inline uint32_t xferProgress() { return _xTotalWritten; }
inline uint32_t xferTotal() { return _xTotal; }
