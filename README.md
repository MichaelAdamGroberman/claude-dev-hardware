# claude-dev-hardware

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: ESP32 / PlatformIO](https://img.shields.io/badge/platform-ESP32%20%2F%20PlatformIO-blue.svg)](https://platformio.org/)

> **This is gr0m's fork of Anthropic's
> [`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy)**,
> licensed MIT (Copyright 2026 Anthropic, PBC — see [LICENSE](LICENSE)). The
> upstream project is a reference implementation of a BLE desk pet for Claude;
> this fork keeps that intact and adds networking, a Claude Code bridge, and an
> MCP control surface (see [What this fork adds](#what-this-fork-adds)).

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions.

As an example, the upstream project built a desk pet on ESP32 that lives off
permission approvals and interaction with Claude. It sleeps when nothing's
happening, wakes when sessions start, gets visibly impatient when an approval
prompt is waiting, and lets you approve or deny right from the device.

The mascot in this fork is the **gr0m 3D vector robot**: a boxy head cube and
chest cube rendered through a real 3×3 rotation matrix, with
accelerometer-driven parallax so the robot appears to turn and look around as
you tilt the device. A horizontal visor stripe carries the mood channel, and
the robot moves through the same set of mood states as the original pet (sleep,
idle, busy, attention, celebrate, dizzy, heart).

<p align="center">
  <img src="docs/device.jpg" alt="M5StickC Plus running the buddy firmware" width="500">
</p>

<p align="center"><em>📖 Full illustrated manual: <a href="docs/manual.html">docs/manual.html</a></em></p>

## What this fork adds

This fork extends the upstream BLE-only desk pet with networking and a
control/automation surface. The capabilities below are what's shipped today —
for setup details see **[tools/cc-bridge/README.md](tools/cc-bridge/README.md)**.

- **WiFi + WireGuard networking.** The BLE stack was migrated from Bluedroid to
  **NimBLE**, which frees ~30–40 KB of heap so WiFi + WireGuard and BLE can
  coexist on the M5StickC Plus's single-radio ESP32.
- **Radio is WiFi _xor_ BLE** (they share the 2.4 GHz radio); **serial is
  always available** regardless of mode. Switch via the Settings menu or with
  `{"cmd":"radio","mode":"wifi"|"bt"|"off"}`.
- **The `tools/cc-bridge` daemon** lets **Claude Code** — both the CLI and the
  Code feature inside the desktop app — show permission prompts on the device
  and approve/deny with the physical buttons. It works over four transports:
  **USB serial, local BLE, WiFi on the same LAN, and WiFi + VPN (WireGuard)**
  for remote reach. See
  [tools/cc-bridge/README.md](tools/cc-bridge/README.md) for setup.
- **The gr0m MCP server** ([`tools/cc-bridge/gr0m_mcp.py`](tools/cc-bridge/gr0m_mcp.py))
  exposes the device to Claude as MCP tools: status, on-screen notify, radio
  mode, owner name, token usage / reporting period / reset, **GPIO
  read/write/mode, ADC read, and a single-channel logic-analyzer capture**, plus
  **adapter mode** — which turns the device into a dedicated GPIO probe.
- **Token-usage reporting on the device.** The daemon computes output-token
  usage over a configurable window (day / week / month / all) from your Claude
  Code transcripts and pushes it to the device for display. Approvals and
  denials are tracked per period alongside it.
- **Character evolution + on-device menus.** The gr0m mascot assembles itself
  across five lifetime-usage milestones (1M → 250M tokens) — from a bare head to
  the full rig (flat-shaded early, dynamically lit once mature). On-device
  menus, opened with a long-press of **A** on the matching info page, cover a
  **Connection** screen (WiFi / BT / Off / Adapter) and a **Usage** screen
  (reporting window, plus reset-counters and reset-buddy-level).
- **DJ mode** — the end-game upgrade. It's **not** a toggle: once the character
  reaches the final evolution stage (Stage 5, 250M lifetime tokens) it breaks
  into a full-detail DJ-booth scene on its own (turntables, mixer, VU-meter
  visor, headphones, note particles on an internal beat) — during celebrations
  and the occasional idle flourish. Earned, not switched on.

> **Design notes:** [docs/superpowers/specs/](docs/superpowers/specs/) has the
> full evolution / DJ / menu design spec.

## Hardware

The firmware targets ESP32 with the Arduino framework. As written, it
depends on the M5StickCPlus library for its display, IMU, and button
drivers—so you'll need that board, or a fork that swaps those drivers for
your own pin layout.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -t upload
```

The build uses **NimBLE** (`h2zero/NimBLE-Arduino`) instead of the default
Bluedroid stack — it's already declared in `platformio.ini`, so PlatformIO
pulls it on first build along with the WireGuard-ESP32 dependency. No extra
setup is needed.

If you're starting from a previously-flashed device, wipe it first to clear
stale NVS:

```bash
pio run -t erase && pio run -t upload
```

Once running, you can also wipe everything from the device itself: **hold A
→ settings → reset → factory reset → tap twice**.

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the stick:

- Make sure it's awake (any button press)
- Check the stick's settings menu → bluetooth is on (remember: the radio is
  WiFi **xor** BLE — if it's in WiFi mode, BLE discovery won't see it)

## Controls

|                         | Normal               | Pet         | Info        | Approval    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A** (front)           | next screen          | next screen | next screen | **approve** |
| **B** (right)           | scroll transcript    | next page   | next page   | **deny**    |
| **Hold A**              | menu                 | menu        | menu        | menu        |
| **Power** (left, short) | toggle screen off    |             |             |             |
| **Power** (left, ~6s)   | hard power off       |             |             |             |
| **Shake**               | dizzy                |             |             | —           |
| **Face-down**           | nap (energy refills) |             |             |             |

The screen auto-powers-off after 30s of no interaction (kept on while an
approval prompt is up). Any button press wakes it.

## ASCII pets

In addition to the gr0m vector robot, the firmware ships the upstream set of
ASCII pets, each with seven animations (sleep, idle, busy, attention,
celebrate, dizzy, heart). Menu → "next pet" cycles them with a counter. Choice
persists to NVS.

## GIF pets

If you want a custom GIF character instead of a built-in buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the stick switches to GIF mode live. **Settings
→ delete char** reverts to the built-in mode.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide; height up to ~140px stays on a 135×240 portrait screen.
Crop tight to the character — transparent margins waste screen and shrink
the sprite. `tools/prep_character.py` handles the resize: feed it source
GIFs at any sizes and it produces a 96px-wide set where the character is the
same scale in every state.

The whole folder must fit under 1.8MB —
`gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.

See `characters/bufo/` for a working example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo` stages it into `data/` and runs
`pio run -t uploadfs` directly over USB.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks**       |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Project layout

```
src/
  main.cpp       — loop, state machine, UI screens
  buddy.cpp      — ASCII species dispatch + render helpers
  buddies/       — one file per species (incl. gr0m), seven anim functions each
  ble_bridge.cpp — Nordic UART service (NimBLE), line-buffered TX/RX
  net_wifi.cpp   — WiFi + WireGuard transport, TCP listener / dial-out
  character.cpp  — GIF decode + render
  data.h         — wire protocol, JSON parse
  xfer.h         — folder push receiver
  stats.h        — NVS-backed stats, settings, owner, species choice
characters/      — example GIF character packs
tools/           — generators, converters, and the cc-bridge daemon + MCP server
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.

## Security

Found a vulnerability? See [SECURITY.md](SECURITY.md) for how to report it
privately.
