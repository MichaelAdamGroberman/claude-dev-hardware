# cc-bridge — Hardware Buddy for Claude Code

Lets your Hardware Buddy device approve/deny `claude-code` CLI tool
calls the same way it does for the Claude desktop app.

Two processes:

| Process | What it does |
|---|---|
| `buddy_bridged.py` | Long-running daemon. Holds the BLE link to the device, exposes a Unix socket at `~/.cache/claude-buddy/buddy.sock` for local clients. |
| `buddy_prompt.py` | One-shot PreToolUse hook. Reads the tool call from stdin, asks the daemon, prints `{"decision":"approve"}` or `{"decision":"block",…}` back to Claude Code. |

## Install

```bash
# 1. Python deps (bleak handles BLE on macOS / Linux / Windows)
pip3 install bleak

# 2. Start the daemon (leave it running)
python3 tools/cc-bridge/buddy_bridged.py
#   stderr + ~/.cache/claude-buddy/buddy.log show "listening on …" and
#   "connecting to Claude-XXXX". Pair on the OS Bluetooth panel the
#   first time — passkey shows on the device.

# 3. Wire the hook into Claude Code
#    Merge tools/cc-bridge/settings.example.json into ~/.claude/settings.json.
#    The example matcher gates Bash + Write + Edit + MultiEdit — adjust to
#    taste. See https://docs.claude.com/en/docs/claude-code/hooks
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
                                  → {"decision":"approve"}   → tool runs
                                  → {"decision":"block",...} → tool blocked
                                  → timeout / no daemon      → silent passthrough
                                                                (Claude shows
                                                                 its own prompt)
```

The bridge tags every prompt with `src:"cli"` so the firmware can render a
small `cli` badge in the alarm bar — that's how you tell a Claude Code
prompt apart from a desktop app prompt at a glance.

## Failure modes

- **Daemon not running** → hook exits silently → Claude Code uses its
  own prompt. The device is never a hard dependency.
- **Device asleep / out of range** → daemon retries with exponential
  backoff (2 s → 60 s). Hook gets `"decision":"disconnected"` and falls
  through to Claude's prompt.
- **User doesn't press A or B** → 30 s timeout (configurable in
  `buddy_prompt.py`). Hook falls through.

## Run as a launchd service (macOS)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.gr0m.claude-buddy</string>
  <key>ProgramArguments</key>
  <array>
    <string>/opt/homebrew/bin/python3</string>
    <string>/Users/michaelgroberman/Downloads/claude-desktop-buddy-main/tools/cc-bridge/buddy_bridged.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key>
  <string>/Users/michaelgroberman/.cache/claude-buddy/buddy.log</string>
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
