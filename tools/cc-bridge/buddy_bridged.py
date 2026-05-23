#!/usr/bin/env python3
"""
buddy-bridged — BLE bridge from Claude Code (or any local hook) to the
Hardware Buddy firmware over BLE Nordic UART Service.

Speaks the protocol in REFERENCE.md:
  - scans for peripherals named "Claude*"
  - sends time + owner on connect
  - sends `{prompt: {...}}` heartbeats when a prompt is requested
  - waits for `{cmd:"permission", id, decision}` back
  - acks `status` polls

Exposes a Unix socket at ~/.cache/claude-buddy/buddy.sock for local
clients (e.g. the buddy-prompt PreToolUse hook). Wire protocol on the
socket is one JSON line per request, one JSON line per response.

Request: {"op":"prompt", "tool":"Bash", "hint":"git push", "src":"cli", "timeout":30}
Reply:   {"decision":"once"}                  // approved
         {"decision":"deny"}                  // denied
         {"decision":"timeout"}               // user didn't decide in time
         {"decision":"disconnected"}          // no device paired

Request: {"op":"status"}
Reply:   {"connected": true/false, "device": "Claude-XXXX"}

Run with:   python3 buddy_bridged.py
Requires:   pip install bleak
"""
from __future__ import annotations

import asyncio
import json
import os
import signal
import sys
import time
import uuid
from pathlib import Path
from typing import Optional

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.device import BLEDevice
except ImportError:
    sys.stderr.write("missing dependency: pip install bleak\n")
    sys.exit(1)

NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # client → device write
NUS_TX  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device → client notify

SOCK_DIR  = Path.home() / ".cache" / "claude-buddy"
SOCK_PATH = SOCK_DIR / "buddy.sock"
LOG_PATH  = SOCK_DIR / "buddy.log"

# How often to send a heartbeat snapshot even when nothing has changed.
HEARTBEAT_S = 10
# How long to wait for a user A/B press on the device before giving up.
DEFAULT_PROMPT_TIMEOUT_S = 30


def log(msg: str) -> None:
    """Append a timestamped line to both stderr and the log file."""
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    sys.stderr.write(line + "\n")
    sys.stderr.flush()
    try:
        with LOG_PATH.open("a") as f:
            f.write(line + "\n")
    except OSError:
        pass


class BuddyLink:
    """Maintains the BLE link and the request/response state machine."""

    def __init__(self) -> None:
        self.client: Optional[BleakClient] = None
        self.device: Optional[BLEDevice] = None
        # Pending prompt: maps prompt id → Future that resolves with the
        # device's decision string ("once" | "deny").
        self.pending: dict[str, asyncio.Future[str]] = {}
        # Line accumulator — BLE notifications can fragment at the MTU.
        self._rx_buf = bytearray()
        # Cached snapshot fields so we can re-send on demand.
        self._owner = "Claude Code"

    # ── BLE plumbing ────────────────────────────────────────────────

    async def scan_and_connect(self) -> bool:
        log("scanning for Claude* peripherals (5s)…")
        devices = await BleakScanner.discover(timeout=5.0)
        candidates = [
            d for d in devices
            if (d.name or "").startswith("Claude")
        ]
        if not candidates:
            log("no Claude* device found")
            return False
        # Pick the first; if you have multiple, prefer one based on local
        # naming convention. We could persist a chosen address in
        # ~/.cache/claude-buddy/device for stickiness — left as a TODO.
        self.device = candidates[0]
        log(f"connecting to {self.device.name} [{self.device.address}]")
        self.client = BleakClient(self.device.address)
        try:
            await self.client.connect()
        except Exception as exc:
            log(f"connect failed: {exc}")
            return False
        await self.client.start_notify(NUS_TX, self._on_notify)
        await self._send_initial()
        return True

    async def _send_initial(self) -> None:
        """One-shot messages the firmware expects on connect."""
        now = int(time.time())
        tz = -time.timezone if time.daylight == 0 else -time.altzone
        await self._send_json({"time": [now, tz]})
        await self._send_json({"cmd": "owner", "name": self._owner})

    async def _send_json(self, obj: dict) -> None:
        if not self.client or not self.client.is_connected:
            return
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        # The NUS RX characteristic accepts writes up to the negotiated
        # MTU. bleak handles chunking when payloads exceed MTU but only
        # if the characteristic supports write-without-response; we use
        # the default (with response) for reliability of acks.
        try:
            await self.client.write_gatt_char(NUS_RX, line, response=True)
        except Exception as exc:
            log(f"write failed: {exc}")

    def _on_notify(self, _char, data: bytearray) -> None:
        """Accumulate bytes, split on \\n, dispatch one line at a time."""
        self._rx_buf.extend(data)
        while b"\n" in self._rx_buf:
            line, _, rest = self._rx_buf.partition(b"\n")
            self._rx_buf = bytearray(rest)
            try:
                msg = json.loads(line.decode("utf-8", errors="replace"))
            except json.JSONDecodeError:
                log(f"bad json from device: {line!r}")
                continue
            asyncio.create_task(self._handle_device_msg(msg))

    async def _handle_device_msg(self, msg: dict) -> None:
        # Permission decision — match it to a pending prompt and resolve.
        if msg.get("cmd") == "permission":
            pid = msg.get("id", "")
            decision = msg.get("decision", "")
            fut = self.pending.pop(pid, None)
            if fut and not fut.done():
                fut.set_result(decision)
                log(f"device → {decision} for {pid}")
            return

        # Status poll — minimal reply. We don't have battery / stats on
        # the bridge side; the firmware sources its own. Just ack.
        if msg.get("cmd") == "status":
            await self._send_json({"ack": "status", "ok": True, "data": {
                "name": "claude-code-bridge",
                "sec": False,
            }})
            return

        if "ack" in msg:
            # Our own ack from the firmware; nothing to do.
            return

        log(f"unhandled device msg: {msg}")

    # ── Prompt API used by the socket server ────────────────────────

    async def request_prompt(
        self, tool: str, hint: str, src: str, timeout_s: float
    ) -> str:
        """Send a permission prompt to the device, return its decision."""
        if not self.client or not self.client.is_connected:
            return "disconnected"
        pid = f"hook_{uuid.uuid4().hex[:10]}"
        fut: asyncio.Future[str] = asyncio.get_event_loop().create_future()
        self.pending[pid] = fut
        # The firmware reads `prompt` out of a heartbeat snapshot; send a
        # minimal one (the other fields stay 0). msg drives the bottom
        # status line for context.
        snapshot = {
            "total": 1, "running": 0, "waiting": 1,
            "msg": f"approve: {tool}",
            "tokens": 0, "tokens_today": 0,
            "prompt": {
                "id": pid, "tool": tool, "hint": hint, "src": src,
            },
        }
        await self._send_json(snapshot)
        log(f"→ device prompt {pid}: {tool} / {hint!r}")
        try:
            return await asyncio.wait_for(fut, timeout=timeout_s)
        except asyncio.TimeoutError:
            self.pending.pop(pid, None)
            # Clear the prompt on the device so it stops showing the
            # alarm bar — send an empty snapshot.
            await self._send_json({
                "total": 0, "running": 0, "waiting": 0, "msg": "",
            })
            return "timeout"

    async def heartbeat_loop(self) -> None:
        while True:
            await asyncio.sleep(HEARTBEAT_S)
            if self.client and self.client.is_connected and not self.pending:
                await self._send_json({
                    "total": 0, "running": 0, "waiting": 0, "msg": "",
                })

    async def disconnect(self) -> None:
        if self.client and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:
                pass


# ── Unix socket server ──────────────────────────────────────────────

async def handle_client(
    link: BuddyLink,
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
) -> None:
    try:
        raw = await reader.readline()
        if not raw:
            return
        try:
            req = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            writer.write(b'{"error":"bad json"}\n'); await writer.drain(); return

        op = req.get("op")
        if op == "status":
            connected = bool(link.client and link.client.is_connected)
            name = link.device.name if link.device else None
            writer.write((json.dumps({
                "connected": connected, "device": name,
            }) + "\n").encode())
        elif op == "prompt":
            tool = str(req.get("tool", "?"))[:19]
            hint = str(req.get("hint", ""))[:43]
            src  = str(req.get("src", "cli"))[:7]
            timeout_s = float(req.get("timeout", DEFAULT_PROMPT_TIMEOUT_S))
            decision = await link.request_prompt(tool, hint, src, timeout_s)
            writer.write((json.dumps({"decision": decision}) + "\n").encode())
        else:
            writer.write(b'{"error":"unknown op"}\n')
        await writer.drain()
    except Exception as exc:
        log(f"client handler error: {exc}")
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def serve_socket(link: BuddyLink) -> None:
    SOCK_DIR.mkdir(parents=True, exist_ok=True)
    if SOCK_PATH.exists():
        SOCK_PATH.unlink()
    server = await asyncio.start_unix_server(
        lambda r, w: handle_client(link, r, w),
        path=str(SOCK_PATH),
    )
    os.chmod(SOCK_PATH, 0o600)
    log(f"listening on {SOCK_PATH}")
    async with server:
        await server.serve_forever()


# ── Main / supervisor loop ──────────────────────────────────────────

async def main() -> None:
    link = BuddyLink()

    def _shutdown(*_a):
        log("shutdown signal — closing")
        asyncio.create_task(link.disconnect())
        for t in asyncio.all_tasks():
            t.cancel()

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, _shutdown)

    # Connect with a retry loop — if the device is asleep or out of range
    # the daemon stays up and keeps trying so hooks fail fast with
    # "disconnected" rather than blocking forever.
    async def reconnector():
        backoff = 2
        while True:
            if not link.client or not link.client.is_connected:
                if await link.scan_and_connect():
                    backoff = 2
                else:
                    log(f"retry in {backoff}s")
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, 60)
                    continue
            await asyncio.sleep(5)

    try:
        await asyncio.gather(
            serve_socket(link),
            link.heartbeat_loop(),
            reconnector(),
        )
    except asyncio.CancelledError:
        pass


if __name__ == "__main__":
    SOCK_DIR.mkdir(parents=True, exist_ok=True)
    asyncio.run(main())
