#pragma once
#include <stdint.h>
#include <stddef.h>

// TCP transport for the buddy bridge over WiFi / WireGuard. Speaks the
// exact same protocol as the BLE Nordic-UART link: newline-delimited JSON
// in, acks/status notifications out. This lets the cc-bridge daemon (and
// therefore Claude Code / the `claude` CLI / Claude Desktop via hooks)
// reach the device over the network — on the home LAN or, over the
// WireGuard tunnel, from anywhere on the tailnet.
//
// Security: a client MUST send the provisioned token as its first line
// before any command is processed; otherwise the connection is dropped.
// The listener does not even start unless a token is set in NVS
// (namespace "buddy", key "tcp_token"). Over the tunnel the traffic is
// additionally encrypted by WireGuard.
void   netTcpInit();                                  // start once WiFi is online
void   netTcpTick();                                  // accept + pump; call each loop
bool   netTcpClientConnected();                       // an authenticated client is attached
size_t netTcpWrite(const uint8_t* data, size_t len);  // mirror outgoing acks to the client
