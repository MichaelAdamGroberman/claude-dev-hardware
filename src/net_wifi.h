#pragma once
#include <stdint.h>

// WiFi station + AP-fallback config portal.
//
// Flow:
//   1. Settings.wifi==true triggers netWifiInit() to read saved SSID/pwd.
//   2. If creds present → STA connect (10s timeout).
//   3. On STA failure OR no creds → AP "gr0m-setup", web portal at
//      192.168.4.1 with a form to enter SSID/password/bridge address.
//   4. Submitting the form persists creds to NVS and reboots the stick.
//
// Subsequent boots skip the portal and go straight to STA mode.

// Full state — used by the indicator UI to show what's happening.
enum NetWifiState {
  NW_OFF,         // disabled in settings, or not initialized
  NW_STARTING,    // softAP/STA in progress
  NW_PORTAL,      // AP mode, config portal serving
  NW_CONNECTING,  // STA mode, waiting for association
  NW_ONLINE,      // STA mode, connected with IP
  NW_FAILED,      // softAP returned false / STA timeout with no creds
};

void        netWifiInit();
void        netWifiStop();
void        netWifiTick();
NetWifiState netWifiState();
bool        netWifiOnline();      // true when STA mode is connected
bool        netWifiPortalActive(); // true when AP-config portal is up
const char* netWifiIP();           // current IP as printable string
const char* netWifiBridgeAddr();   // configured Mac-bridge "host:port", "" if none
const char* netWifiLastError();    // most recent failure reason, "" if none
int         netWifiApClients();    // # stations associated to the setup AP (live)
