# cc-bridge — Hardware Buddy for Claude Code

Lets your Hardware Buddy device approve/deny `claude-code` CLI tool
calls the same way it does for the Claude desktop app.

Three pieces:

| Process | What it does |
|---|---|
| `buddy_bridged.py` | Long-running daemon. Holds one link to the device (serial / BLE / WiFi), exposes a Unix socket at `~/.cache/claude-buddy/buddy.sock` for local clients. |
| `buddy_prompt.py` | One-shot PreToolUse hook. Reads the tool call from stdin, mirrors input-required questions display-only, and asks the daemon for binary approve/deny only in ask-permission/default mode. |
| `gr0m_mcp.py` | MCP server (stdio). Lets Claude **control/query** the device — status, notify, radio mode, owner, token usage, and **GPIO / logic-analyzer** — via tools. Thin client of the daemon socket. See [gr0m MCP server](#gr0m-mcp-server). |

## Install

```bash
# 1. Python deps — bleak for BLE mode, pyserial for serial mode.
pip3 install bleak pyserial          # gr0m_mcp.py needs neither

# 2. Start the daemon (leave it running). Default transport is BLE; see
#    Transports below for serial / WiFi / VPN via env vars.
python3 tools/cc-bridge/buddy_bridged.py
#   stderr + ~/.cache/claude-buddy/buddy.log show "listening on …" and the
#   link state. For BLE, pair on the OS Bluetooth panel the first time —
#   passkey shows on the device.

# 3. Wire the hook into Claude Code (approve/deny on the device)
#    Merge tools/cc-bridge/settings.example.json into ~/.claude/settings.json.
#    The example matcher gates Bash + Write + Edit + MultiEdit — adjust to
#    taste. See https://docs.claude.com/en/docs/claude-code/hooks

# 4. (Optional) Register the gr0m MCP server to control/query the device
#    from Claude. Add it to ~/.claude.json — see "gr0m MCP server" below.
```

## How it works

```
claude-code ── PreToolUse hook ──► buddy_prompt.py
                                       │  JSON over Unix socket
                                       ▼
                                  buddy_bridged.py
                                       │  BLE NUS (Nordic UART Service)
                                       ▼
                                  Hardware Buddy
                                       │  A=approve / B=deny on physical buttons
                                       ▼
                                  Hook resolves
                                  → permissionDecision:"allow" → tool runs
                                  → permissionDecision:"deny"  → tool blocked
                                  → timeout / no daemon      → silent passthrough
                                                                (Claude shows
                                                                 its own prompt)
```

The bridge tags every prompt with `src:"cli"` so the firmware can render a
small `cli` badge in the alarm bar — that's how you tell a Claude Code
prompt apart from a desktop app prompt at a glance.

The hook only sends binary approval prompts to the device when Claude Code is
running in ask-permission/default mode. Auto-run modes such as `acceptEdits`,
`bypassPermissions`, and `plan` pass through silently. `AskUserQuestion` still
mirrors the question text to the device as display-only because the actual
choice is made in Claude Code.

## Transports — serial / local BT / WiFi / VPN

The daemon reaches the device over one of four transports, chosen by
environment variable. The Unix-socket contract to the hook (and the MCP
server) is identical across all of them, so nothing on the Claude side
changes.

| Where you are | Env on the daemon | Path |
|---|---|---|
| **USB serial** | `BUDDY_SERIAL=/dev/cu.usbserial-XXXX` | one wired link, no pairing — most reliable on a desk |
| **local BT** (default) | *(none)* | scans for `Claude*`, BLE NUS |
| **WiFi, same LAN** | `BUDDY_HOST=<device-ip>:6400` `BUDDY_TOKEN=<tok>` | daemon dials the device's TCP listener |
| **WiFi + VPN** (remote) | `BUDDY_LISTEN=0.0.0.0:6401` `BUDDY_TOKEN=<tok>` | device **dials out** to the daemon over the WireGuard tunnel |

> The device's radio is WiFi **xor** BLE (shared 2.4 GHz). Serial works in
> any mode. Switch with `{"cmd":"radio","mode":"wifi"|"bt"|"off"}` (or the
> Settings menu). **The daemon holds the serial port** in `BUDDY_SERIAL`
> mode — stop it before flashing: `launchctl bootout
> gui/$(id -u)/com.gr0m.claude-buddy`.

The device's WiFi/WG support and the `tcp_token` (required for any network
transport) are provisioned with a `wifi` command — most reliably sent over
an already-working link (BLE or the TCP listener), since serial delivery is
timing-sensitive:

```jsonc
{"cmd":"wifi",
 "nets":[{"ssid":"home","pwd":"…"},{"ssid":"hotspot","pwd":"…"}],
 "wg":"<wg-quick config>",          // optional: brings up the tunnel
 "token":"<random hex>",            // enables the TCP listener (:6400)
 "peer":"<bridge-tailnet-ip>:6401", // optional: enables dial-out (VPN)
 "apply":true}                      // writes NVS + reboots
```

**Why the VPN path dials out:** the lightweight WireGuard-ESP32 stack
carries device-*initiated* traffic but doesn't accept inbound connections
to the tunnel IP. So for remote reach, point `peer` at the daemon's tailnet
IP and run the daemon with `BUDDY_LISTEN`; the device opens the connection
outward. On the LAN you can use either mode (the listener at `:6400` works
for inbound there).

## Claude Desktop

The **Claude Desktop chat app shows tool prompts on the device natively
over BLE** — that's the buddy's built-in purpose, no bridge needed (just
re-pair once after a firmware update, since pairing keys reset). Desktop
does **not** support PreToolUse hooks, and its native buddy link is
BLE-only, so Desktop can't be routed over WiFi/VPN. For on-device approval
over the network, use **Claude Code** (CLI or the Claude Code feature
inside the desktop app), which this bridge serves over all three transports.

## gr0m MCP server

`gr0m_mcp.py` exposes the device to Claude as MCP tools, so Claude (or you,
in chat) can drive it directly. It's a dependency-free stdio MCP server and
a thin client of the daemon socket — it works over whatever transport the
daemon is using.

Register it in `~/.claude.json` under `mcpServers` (restart Claude Code to
load it):

```jsonc
"mcpServers": {
  "gr0m": {
    "command": "/usr/local/bin/python3",
    "args": ["/path/to/tools/cc-bridge/gr0m_mcp.py"]
  }
}
```

Tools:

| Tool | Args | Effect |
|---|---|---|
| `gr0m_status` | — | connection, transport, battery, token usage + current period |
| `gr0m_notify` | `message` | show a line on the device screen |
| `gr0m_set_radio` | `mode` = `wifi`\|`bt`\|`off` | switch radio mode (reboots the device) |
| `gr0m_set_owner` | `name` | set the on-screen owner name |
| `gr0m_token_reset` | — | zero the token-usage counter |
| `gr0m_token_period` | `period` = `day`\|`week`\|`month`\|`all` | set the usage reporting window |
| `gr0m_gpio_read` | `pin` | digital read |
| `gr0m_gpio_write` | `pin`, `value` | drive a pin high/low |
| `gr0m_gpio_mode` | `pin`, `mode` = `input`\|`output`\|`pullup` | set pin mode |
| `gr0m_adc_read` | `pin` | analog read → raw (0-4095) + mV |
| `gr0m_logic_capture` | `pin`, `samples`, `interval_us` | mini logic-analyzer trace |
| `gr0m_adapter` | `on` = `true`\|`false` | adapter mode: dedicate the device to GPIO probing |
| `gr0m_level_reset` | — | reset the buddy's lifetime level → de-evolve to Stage 0 |

`gr0m_status` now also reports `level`, `evoStage` (0–5), `lifetimeTokens`,
`periodTokens`, and `approved`/`denied` for the current window. `gr0m_token_reset`
clears the approved/denied tally along with the token counter. The same controls
(reporting window, reset counters, reset level) are available on-device in the
**Usage** menu, and radio/adapter in the **Connection** menu (long-press **A** on
the matching info page).

### GPIO / logic analyzer

The device exposes its header pins as a probe over MCP — read/write/mode,
analog read, and a single-channel **logic-analyzer capture** (sample one pin
at a fixed interval, up to 512 samples, returned as a hex bit-trace with the
actual elapsed time so you can derive the real rate). Restricted to the
exposed pins that aren't wired to the display/IMU/buttons/IR/mic — **0, 25,
26, 32, 33, 36** (36 is input-only; ADC-capable: 32, 33, 36) — so a probe
can't brick the device. Firmware command: `{"cmd":"gpio","act":...}`; the
daemon's `op:query` waits for the `{"ack":"gpio",...}` reply and returns it.

**Adapter mode** (`gr0m_adapter`) turns the buddy into a dedicated probe: it
stops the pet animation + mic and quiets BLE advertising so the device is
focused on GPIO/logic work, while the command transports keep running. It's
**in-memory only — a device reset returns it to normal BT/WiFi pet mode**
(or call `gr0m_adapter(false)`). It can also be toggled **on the device**:
Settings → `adapter` to enter, **BtnB** (or a reset) to exit.

### Token usage reporting

The daemon computes **output-token usage** over the selected window
(`day`/`week`/`month`/`all`) from your Claude Code transcripts
(`~/.claude/projects/**/*.jsonl`) and pushes it to the device every
heartbeat as `{"cmd":"tokens","set":N,"life":N,"ok":N,"deny":N}`. The
firmware shows the period count on the chest LCD/stats, while lifetime drives
level/evolution and ok/deny show the current-period approval tally.
`gr0m_token_reset` records a baseline and the counter restarts from zero.
Period + reset state persists in `~/.cache/claude-buddy/token_state.json`;
the default window is set with `BUDDY_TOKEN_PERIOD` on the daemon.

## Failure modes

- **Daemon not running** → hook exits silently → Claude Code uses its
  own prompt. The device is never a hard dependency.
- **Device asleep / out of range** → daemon retries with exponential
  backoff (2 s → 60 s). Hook gets `"decision":"disconnected"` and falls
  through to Claude's prompt.
- **User doesn't press A or B** → 30 s timeout (configurable in
  `buddy_prompt.py`). Hook falls through.
- **Daemon up but delivering nothing** → check `~/.cache/claude-buddy/buddy.log`
  for a `scanning for Claude* (5 s)…` line with *no outcome after it*. That was
  a deadlocked reconnector (fixed; see `test_reconnect.py`). Also check for
  `CBATTErrorDomain Code=15 "Encryption is insufficient"` — the NUS
  characteristic needs a bond, so the link is up but unpaired. Re-pair, or use
  serial, which needs no bond.

## Tests

`test_reconnect.py` covers the connect / reconnect / liveness paths offline —
no hardware and no BLE adapter, via a fake `bleak` and a throwaway `$HOME`.

```bash
./test_reconnect.py              # all scenarios + summary
./test_reconnect.py ble_hang     # one scenario
```

The other `tools/test_*.py` scripts are hardware-in-the-loop; this one is not.

## Run as a launchd service (macOS)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.gr0m.claude-buddy</string>
  <key>ProgramArguments</key>
  <array>
    <!-- python with pyserial (serial mode) and/or bleak (BLE mode) -->
    <string>/opt/homebrew/bin/python3.14</string>
    <string>/path/to/claude-dev-hardware/tools/cc-bridge/buddy_bridged.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key>
  <string>/Users/YOUR_USERNAME/.cache/claude-buddy/buddy.log</string>
  <!-- Pick ONE transport. Default (no transport var) = BLE. -->
  <key>EnvironmentVariables</key>
  <dict>
    <key>BUDDY_OWNER</key><string>gr0m</string>
    <key>BUDDY_TOKEN_PERIOD</key><string>day</string>
    <key>BUDDY_SERIAL</key><string>/dev/cu.usbserial-XXXX</string>
    <!-- WiFi (same LAN):  <key>BUDDY_HOST</key><string>192.168.1.20:6400</string> -->
    <!-- VPN (device dials in): <key>BUDDY_LISTEN</key><string>0.0.0.0:6401</string> -->
    <!-- TCP transports also need: <key>BUDDY_TOKEN</key><string>…</string> -->
  </dict>
</dict>
</plist>
```

Save as `~/Library/LaunchAgents/com.gr0m.claude-buddy.plist`, then
`launchctl load ~/Library/LaunchAgents/com.gr0m.claude-buddy.plist`.

## What about Claude mobile?

Not supported. The mobile app doesn't expose tool-permission prompts to
third-party processes, and iOS Core Bluetooth blocks generic BLE access
from outside the owning app. You'd need a native iOS / Android app to
make this work, which is outside the scope of this bridge.
