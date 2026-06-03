#pragma once
// WireGuard tunnel over the existing WiFi STA link. Lets the device
// be reachable on a tailnet (or any WireGuard subnet) from off-LAN
// clients without exposing the BLE bridge to the open internet.
//
// Workflow:
//   1. User pastes a standard wg-quick config into the WiFi portal.
//      portal handler parses [Interface] + [Peer], stores keys + ips
//      in NVS under namespace "buddy" with keys wg_*.
//   2. netWgInit() runs after netWifiOnline() goes true. Reads NVS,
//      calls WireGuard.begin() on the ciniml/WireGuard-ESP32 lib.
//   3. netWgTick() is called from the main loop. The lib does its
//      own packet pump; we just expose status.
//
// State machine mirrors netWifi:
//   OFF       — no config in NVS, or settings().wg is off
//   STARTING  — keys loaded, handshake in progress
//   UP        — peer handshake completed, tunnel carrying packets
//   FAILED    — handshake never completed (peer offline / bad keys)

enum NetWgState {
  WG_OFF,
  WG_STARTING,
  WG_UP,
  WG_FAILED,
};

void        netWgInit();
void        netWgStop();
void        netWgTick();
NetWgState  netWgState();
const char* netWgTunnelIP();     // our address on the tunnel, e.g. 100.64.5.10
const char* netWgPeerEndpoint(); // peer host:port we're tunneling to
const char* netWgLastError();    // most recent failure reason, "" if none

// Convenience for the portal handler: pastes a multi-line wg-quick
// config into NVS, returns true if it parsed and saved cleanly.
// Format expected (any extra lines are ignored):
//   [Interface]
//   PrivateKey = <base64>
//   Address    = 10.20.30.5/24
//   [Peer]
//   PublicKey  = <base64>
//   Endpoint   = host.example.com:51820
//   AllowedIPs = 0.0.0.0/0
bool        netWgSaveConfigFromText(const char* text);
