#!/usr/bin/env python3
"""
buddy-bridged — bridge from Claude Code / the `claude` CLI (or any local
hook) to the Hardware Buddy firmware. Speaks the Nordic-UART JSON protocol
over either transport:

  • BLE   (default)            — local Bluetooth, scans for "Claude*".
  • TCP   (BUDDY_HOST set)     — WiFi / WireGuard. Connects to the device's
                                 token-gated listener at host:port (6400).

Transport is chosen by environment:
  BUDDY_HOST   "10.0.0.33:6400" (LAN) or "10.20.30.5:6400" (over the VPN)
               → use TCP. Unset → use BLE.
  BUDDY_TOKEN  shared token the device requires for TCP (see provisioning).
  BUDDY_OWNER  name shown on the device ("CLI" by default).

Exposes a Unix socket at ~/.cache/claude-buddy/buddy.sock for local
clients (the buddy-prompt PreToolUse hook). One JSON line per request,
one per response — unchanged across transports.

Request: {"op":"prompt","tool":"Bash","hint":"git push","src":"cli","timeout":30}
Reply:   {"decision":"once"|"deny"|"timeout"|"disconnected"}
Request: {"op":"status"}
Reply:   {"connected":true/false,"device":"Claude-XXXX"|"tcp:host:port"}

Requires:  pip install bleak   (only needed for the BLE transport)
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

NUS_RX  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # client → device write
NUS_TX  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device → client notify

SOCK_DIR  = Path.home() / ".cache" / "claude-buddy"
SOCK_PATH = SOCK_DIR / "buddy.sock"
LOG_PATH  = SOCK_DIR / "buddy.log"

HEARTBEAT_S = 10
DEFAULT_PROMPT_TIMEOUT_S = 30


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    sys.stderr.write(line + "\n"); sys.stderr.flush()
    try:
        with LOG_PATH.open("a") as f:
            f.write(line + "\n")
    except OSError:
        pass


class BuddyLink:
    """Holds the device link (BLE or TCP) and the prompt state machine."""

    def __init__(self) -> None:
        self.pending: dict[str, asyncio.Future[str]] = {}
        self._rx_buf = bytearray()
        self._owner = os.environ.get("BUDDY_OWNER", "CLI")

        # Transport selection:
        #   BUDDY_LISTEN set → wait for the device to dial in (VPN/off-LAN).
        #   BUDDY_HOST set   → dial the device's listener (LAN).
        #   neither          → BLE.
        self._host   = os.environ.get("BUDDY_HOST", "").strip()
        self._listen = os.environ.get("BUDDY_LISTEN", "").strip()
        self._token  = os.environ.get("BUDDY_TOKEN", "").strip()
        self.transport = ("tcp-listen" if self._listen
                          else "tcp" if self._host else "ble")

        # BLE state.
        self.client = None   # BleakClient
        self.device = None   # BLEDevice
        # TCP state.
        self._tcp_reader: Optional[asyncio.StreamReader] = None
        self._tcp_writer: Optional[asyncio.StreamWriter] = None

    # ── transport-agnostic helpers ──────────────────────────────────

    def is_connected(self) -> bool:
        if self.transport == "ble":
            return bool(self.client and self.client.is_connected)
        return self._tcp_writer is not None and not self._tcp_writer.is_closing()

    @property
    def device_name(self) -> Optional[str]:
        if self.transport == "tcp":
            return f"tcp:{self._host}"
        if self.transport == "tcp-listen":
            return f"tcp-listen:{self._listen}"
        return self.device.name if self.device else None

    async def connect(self) -> bool:
        if self.transport == "tcp":
            return await self._tcp_connect()
        if self.transport == "tcp-listen":
            return self.is_connected()   # passive — the listener fills the link
        return await self._ble_connect()

    async def _send_initial(self) -> None:
        now = int(time.time())
        tz = -time.timezone if time.daylight == 0 else -time.altzone
        await self._send_json({"time": [now, tz]})
        await self._send_json({"cmd": "owner", "name": self._owner})

    async def _send_json(self, obj: dict) -> None:
        if not self.is_connected():
            return
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            if self.transport == "ble":
                await self.client.write_gatt_char(NUS_RX, line, response=True)
            else:
                self._tcp_writer.write(line)
                await self._tcp_writer.drain()
        except Exception as exc:
            log(f"write failed: {exc}")

    def _feed(self, data: bytes) -> None:
        """Accumulate bytes, split on newline, dispatch each line."""
        self._rx_buf.extend(data)
        while b"\n" in self._rx_buf:
            line, _, rest = self._rx_buf.partition(b"\n")
            self._rx_buf = bytearray(rest)
            try:
                msg = json.loads(line.decode("utf-8", errors="replace"))
            except json.JSONDecodeError:
                continue
            asyncio.create_task(self._handle_device_msg(msg))

    async def _handle_device_msg(self, msg: dict) -> None:
        if msg.get("cmd") == "permission":
            pid = msg.get("id", ""); decision = msg.get("decision", "")
            fut = self.pending.pop(pid, None)
            if fut and not fut.done():
                fut.set_result(decision)
                log(f"device → {decision} for {pid}")
            return
        if msg.get("cmd") == "status":
            await self._send_json({"ack": "status", "ok": True,
                                   "data": {"name": "claude-code-bridge", "sec": False}})
            return
        if "ack" in msg or "auth" in msg:
            return
        log(f"unhandled device msg: {msg}")

    # ── BLE transport ───────────────────────────────────────────────

    async def _ble_connect(self) -> bool:
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError:
            log("BLE transport needs bleak: pip install bleak")
            return False
        log("scanning for Claude* peripherals (5s)…")
        devices = await BleakScanner.discover(timeout=5.0)
        cands = [d for d in devices if (d.name or "").startswith("Claude")]
        if not cands:
            log("no Claude* device found"); return False
        self.device = cands[0]
        log(f"connecting to {self.device.name} [{self.device.address}]")
        self.client = BleakClient(self.device.address)
        try:
            await self.client.connect()
        except Exception as exc:
            log(f"connect failed: {exc}"); return False
        await self.client.start_notify(NUS_TX, lambda _c, d: self._feed(bytes(d)))
        await self._send_initial()
        return True

    # ── TCP transport (WiFi / WireGuard) ────────────────────────────

    async def _tcp_connect(self) -> bool:
        host, _, port_s = self._host.partition(":")
        port = int(port_s or "6400")
        log(f"connecting TCP to {host}:{port}…")
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=6)
        except Exception as exc:
            log(f"tcp connect failed: {exc}"); return False
        self._tcp_reader, self._tcp_writer = reader, writer
        # Authenticate: the device requires the token as the first line.
        if self._token:
            writer.write((self._token + "\n").encode()); await writer.drain()
            try:
                auth = await asyncio.wait_for(reader.readline(), timeout=5)
            except asyncio.TimeoutError:
                log("tcp auth timeout"); await self._tcp_close(); return False
            if b'"ok"' not in auth:
                log(f"tcp auth rejected: {auth!r}"); await self._tcp_close(); return False
            log("tcp authenticated")
        else:
            log("warning: BUDDY_TOKEN unset — device will drop the connection")
        asyncio.create_task(self._tcp_read_pump())
        await self._send_initial()
        return True

    async def _tcp_read_pump(self) -> None:
        try:
            while self._tcp_reader and not self._tcp_reader.at_eof():
                data = await self._tcp_reader.read(512)
                if not data:
                    break
                self._feed(data)
        except Exception as exc:
            log(f"tcp read pump ended: {exc}")
        await self._tcp_close()

    async def _tcp_close(self) -> None:
        w, self._tcp_writer, self._tcp_reader = self._tcp_writer, None, None
        if w:
            try:
                w.close()
            except Exception:
                pass

    async def serve_listener(self) -> None:
        """Listen-mode: the device dials IN (used over the VPN, where the
        device can't accept inbound). First line from the device must be the
        token; then that socket becomes the device link."""
        host, _, port_s = self._listen.partition(":")
        port = int(port_s or "6401")

        async def on_device(reader, writer):
            peer = writer.get_extra_info("peername")
            try:
                tok = (await asyncio.wait_for(reader.readline(), timeout=5)).decode().strip()
            except (asyncio.TimeoutError, Exception):
                writer.close(); return
            if self._token and tok != self._token:
                log(f"device dial-in auth failed from {peer}")
                writer.close(); return
            if self.is_connected():
                log(f"device already linked — dropping dial-in from {peer}")
                writer.close(); return
            self._tcp_reader, self._tcp_writer = reader, writer
            log(f"device dialed in from {peer}")
            await self._send_initial()
            try:
                while not reader.at_eof():
                    data = await reader.read(512)
                    if not data:
                        break
                    self._feed(data)
            except Exception as exc:
                log(f"device link ended: {exc}")
            await self._tcp_close()

        server = await asyncio.start_server(on_device, host or "0.0.0.0", port)
        log(f"awaiting device dial-in on {host or '0.0.0.0'}:{port}")
        async with server:
            await server.serve_forever()

    # ── prompt API (transport-agnostic) ─────────────────────────────

    async def request_prompt(self, tool: str, hint: str, src: str, timeout_s: float) -> str:
        if not self.is_connected():
            return "disconnected"
        pid = f"hook_{uuid.uuid4().hex[:10]}"
        fut: asyncio.Future[str] = asyncio.get_event_loop().create_future()
        self.pending[pid] = fut
        await self._send_json({
            "total": 1, "running": 0, "waiting": 1,
            "msg": f"approve: {tool}", "tokens": 0, "tokens_today": 0,
            "prompt": {"id": pid, "tool": tool, "hint": hint, "src": src},
        })
        log(f"→ device prompt {pid}: {tool} / {hint!r}")
        try:
            return await asyncio.wait_for(fut, timeout=timeout_s)
        except asyncio.TimeoutError:
            self.pending.pop(pid, None)
            await self._send_json({"total": 0, "running": 0, "waiting": 0, "msg": ""})
            return "timeout"

    async def heartbeat_loop(self) -> None:
        while True:
            await asyncio.sleep(HEARTBEAT_S)
            if self.is_connected() and not self.pending:
                await self._send_json({"total": 0, "running": 0, "waiting": 0, "msg": ""})

    async def disconnect(self) -> None:
        if self.transport == "tcp":
            await self._tcp_close()
        elif self.client and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:
                pass


# ── Unix socket server (unchanged contract) ─────────────────────────

async def handle_client(link: BuddyLink, reader, writer) -> None:
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
            writer.write((json.dumps({
                "connected": link.is_connected(), "device": link.device_name,
            }) + "\n").encode())
        elif op == "prompt":
            decision = await link.request_prompt(
                str(req.get("tool", "?"))[:19],
                str(req.get("hint", ""))[:43],
                str(req.get("src", "cli"))[:7],
                float(req.get("timeout", DEFAULT_PROMPT_TIMEOUT_S)),
            )
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
        lambda r, w: handle_client(link, r, w), path=str(SOCK_PATH))
    os.chmod(SOCK_PATH, 0o600)
    log(f"listening on {SOCK_PATH} (transport={link.transport})")
    async with server:
        await server.serve_forever()


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

    async def reconnector():
        backoff = 2
        while True:
            if not link.is_connected():
                if await link.connect():
                    backoff = 2
                else:
                    log(f"retry in {backoff}s")
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, 60)
                    continue
            await asyncio.sleep(5)

    tasks = [serve_socket(link), link.heartbeat_loop()]
    tasks.append(link.serve_listener() if link.transport == "tcp-listen"
                 else reconnector())
    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass


if __name__ == "__main__":
    SOCK_DIR.mkdir(parents=True, exist_ok=True)
    asyncio.run(main())
