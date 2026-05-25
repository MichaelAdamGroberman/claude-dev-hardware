#include "ble_bridge.h"
#include "net_tcp.h"     // mirror outgoing acks to the TCP client too
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

// Nordic UART Service UUIDs — every BLE serial example uses these, so
// existing tools (nRF Connect, bluefy, Web Bluetooth examples) can talk to
// us without custom UUIDs.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Incoming bytes are buffered in a simple ring for bleRead()/bleAvailable().
// Sized to hold a transcript snapshot JSON plus headroom; the GATT layer
// will flow-control if we fall behind.
static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static NimBLEServer*         server = nullptr;
static NimBLECharacteristic* txChar = nullptr;
static NimBLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;
static volatile uint16_t  connHandle = BLE_HS_CONN_HANDLE_NONE;
// Radio mutex: when suspended, the RX path drops every incoming byte so a
// still-connected peer can't keep feeding commands while WiFi owns the
// radio. Set via bleSetSuspended() from the radio-mode logic.
static volatile bool      suspended = false;
// Advertising state — declared here (above ServerCallbacks) because the
// onDisconnect callback reads/writes it for the radio-mutex guard.
static bool _advertising = true;

static void rxPush(const uint8_t* p, size_t n) {
  if (suspended) return;   // radio mutex — WiFi owns the radio; ignore BLE RX
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;  // full — drop (upstream should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

void bleInjectRx(const uint8_t* data, size_t len) { rxPush(data, len); }

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override {
    std::string v = c->getValue();
    if (!v.empty()) rxPush((const uint8_t*)v.data(), v.size());
  }
};

// In NimBLE the server callbacks also carry the security/pairing events.
// LE Secure Connections, passkey display: we are DisplayOnly, the central
// is KeyboardOnly. The stack asks us for a passkey to show; we generate a
// random 6-digit one and main.cpp polls blePasskey() to render it.
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& info) override {
    connected = true;
    connHandle = info.getConnHandle();
    Serial.println("[ble] connected");
    // Request a tighter connection interval so prompt/ack round-trips don't
    // wait out the central's default (often 30-50ms+). macOS may not honor
    // it exactly, but it will negotiate down toward this window.
    //   minInterval 12  -> 12 * 1.25ms = 15ms
    //   maxInterval 24  -> 24 * 1.25ms = 30ms
    //   latency     0   -> no skipped connection events
    //   timeout     400 -> 400 * 10ms  = 4000ms supervision timeout
    const uint16_t kMinInterval = 12;
    const uint16_t kMaxInterval = 24;
    const uint16_t kLatency     = 0;
    const uint16_t kTimeout     = 400;
    if (pServer) {
      pServer->updateConnParams(connHandle, kMinInterval, kMaxInterval,
                                kLatency, kTimeout);
      Serial.printf("[ble] req conn params min=%u max=%u lat=%u to=%u\n",
                    kMinInterval, kMaxInterval, kLatency, kTimeout);
    }
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    Serial.printf("[ble] disconnected (reason=0x%x)\n", reason);
    // Restart advertising so the next client can find us — UNLESS BLE is
    // suspended for the radio mutex (WiFi owns the radio). Without this guard,
    // bleSetSuspended(true)'s forced disconnect would land here and immediately
    // re-advertise, defeating the mutex. _advertising is updated to match so
    // bleAdvertising() stays truthful.
    if (suspended) { _advertising = false; return; }
    NimBLEDevice::startAdvertising();
    _advertising = true;
  }
  void onMTUChange(uint16_t newMtu, NimBLEConnInfo& /*info*/) override {
    mtu = newMtu;
    Serial.printf("[ble] mtu=%u\n", mtu);
  }
  uint32_t onPassKeyDisplay() override {
    uint32_t pk = esp_random() % 1000000;
    passkey = pk;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)pk);
    return pk;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    passkey = 0;
    secure = info.isEncrypted();
    Serial.printf("[ble] auth %s\n", secure ? "ok" : "FAIL");
    if (!secure && server && connHandle != BLE_HS_CONN_HANDLE_NONE) {
      server->disconnect(connHandle);
    }
  }
};

void bleInit(const char* deviceName) {
  NimBLEDevice::init(deviceName);
  // Request the biggest MTU we can get. macOS negotiates to 185 typically.
  NimBLEDevice::setMTU(517);

  // LE Secure Connections + bonding + MITM (passkey-display pairing).
  NimBLEDevice::setSecurityAuth(/*bonding*/true, /*mitm*/true, /*sc*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

  // TX (device -> client) notifies. The link is encrypted, so the payload
  // is protected; NimBLE auto-creates the CCCD for a NOTIFY characteristic.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC
  );

  // RX (client -> device) writes require an encrypted (bonded) link.
  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  NimBLEDevice::deleteAllBonds();
  Serial.println("[ble] cleared all bonds");
}

void bleAdvertisingStart() {
  // Radio mutex: refuse to advertise while BLE is suspended (WiFi owns the
  // radio). bleSetSuspended(false) clears `suspended` before it calls in
  // here, so the legitimate resume path still works.
  if (suspended) return;
  if (_advertising) return;
  // `server` is non-null only after bleInit() — in WiFi-only boots the stack
  // is never initialized, so guard the NimBLE call to avoid touching an
  // uninitialized host.
  if (server) NimBLEDevice::startAdvertising();
  _advertising = true;
  Serial.println("[ble] advertising started");
}

void bleAdvertisingStop() {
  if (!_advertising) return;
  if (server) NimBLEDevice::stopAdvertising();   // no-op if stack not up
  _advertising = false;
  Serial.println("[ble] advertising stopped");
}

bool bleAdvertising() { return _advertising; }

void bleSetSuspended(bool s) {
  if (suspended == s) return;
  suspended = s;
  if (s) {
    // Stop advertising so no new central can attach, and force-drop any live
    // connection so an already-paired peer can't keep writing commands while
    // WiFi owns the radio. rxPush() also short-circuits while suspended, so
    // even an in-flight write is dropped rather than dispatched.
    bleAdvertisingStop();
    if (server && connHandle != BLE_HS_CONN_HANDLE_NONE) {
      server->disconnect(connHandle);
    }
    Serial.println("[ble] suspended (radio handed to WiFi)");
  } else {
    bleAdvertisingStart();
    Serial.println("[ble] resumed (radio back to BLE)");
  }
}

bool bleSuspended() { return suspended; }

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  // Mirror the whole payload to the TCP bridge client (if attached). TCP
  // has no MTU limit and is independent of whether a BLE client is linked,
  // so acks/status reach a network client even with no BLE peer.
  netTcpWrite(data, len);
  if (!connected || !txChar) return 0;
  // ATT notify payload is limited to (MTU - 3). macOS negotiates 185, so
  // the 182-byte chunk works there; use the live mtu so a peer that caps
  // at the 23-byte default doesn't get truncated notifies.
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    // Small yield so the BLE stack flushes before the next chunk.
    delay(4);
  }
  return sent;
}
