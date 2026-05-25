#!/usr/bin/env python3
"""
buddy-bridged — bridge from Claude Code / the `claude` CLI / the gr0m MCP
server to the Hardware Buddy firmware. Speaks the Nordic-UART JSON protocol
over one of four transports, chosen by environment:

  BUDDY_SERIAL "/dev/cu.usbserial-XXXX"  → USB serial (reliable, no pairing)
  BUDDY_LISTEN "0.0.0.0:6401"            → device dials in (WiFi+VPN)
  BUDDY_HOST   "10.0.0.33:6400"          → dial the device (WiFi LAN)
  (none)                                 → BLE (scans for Claude*)
  BUDDY_TOKEN  shared token for the TCP transports
  BUDDY_OWNER  on-screen name ("CLI")
  BUDDY_TOKEN_PERIOD  day|week|month|all (default day) — token usage window

Unix socket ~/.cache/claude-buddy/buddy.sock, one JSON line per req/resp:
  {"op":"prompt","tool":..,"hint":..,"src":..,"timeout":..} -> {"decision":..}
  {"op":"status"}                 -> {connected, device, transport,
                                      periodTokens, lifetimeTokens, level,
                                      evoStage, approved, denied, period}
  {"op":"send","cmd":{...}}       -> forward a raw command to the device
  {"op":"clearprompt"}            -> send {"cmd":"clearprompt"} to device; {"ok":true}
  {"op":"token","action":"reset"} -> zero the period counter + approved/denied
  {"op":"token","action":"period","value":"day|week|month|all"}
  {"op":"token","action":"level_reset"} -> zero the lifetime baseline
"""
from __future__ import annotations

import asyncio
import json
import os
import signal
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

SOCK_DIR   = Path.home() / ".cache" / "claude-buddy"
SOCK_PATH  = SOCK_DIR / "buddy.sock"
LOG_PATH   = SOCK_DIR / "buddy.log"
STATE_PATH = SOCK_DIR / "token_state.json"
PROJECTS_DIR = Path.home() / ".claude" / "projects"

HEARTBEAT_S = 10
DEFAULT_PROMPT_TIMEOUT_S = 30

# Evolution stage milestones (lifetime tokens → stage). Stages 1–4 every ~5
# levels (250K); Stage 5 (Ascended) is a far-off endgame at 250M. Matches
# deployment/evolution/SPEC.md and src/stats.h EVO_MILESTONES.
_EVO_MILESTONES = [250_000_000, 1_000_000, 750_000, 500_000, 250_000]


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    sys.stderr.write(line + "\n"); sys.stderr.flush()
    try:
        with LOG_PATH.open("a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def _period_start(period: str) -> float:
    """Epoch (UTC) for the start of the reporting window."""
    now = datetime.now(timezone.utc)
    if period == "all":
        return 0.0
    if period == "week":
        return (now.timestamp()) - 7 * 86400
    if period == "month":
        return (now.timestamp()) - 30 * 86400
    # day = since local midnight
    local = datetime.now()
    midnight = local.replace(hour=0, minute=0, second=0, microsecond=0)
    return midnight.timestamp()


def _evo_stage(lifetime: int) -> int:
    """Map lifetime token count to evolution stage 0–5 (highest milestone crossed)."""
    stage = 0
    for s, milestone in enumerate(reversed(_EVO_MILESTONES), start=1):
        if lifetime >= milestone:
            stage = s
    return stage


class BuddyLink:
    def __init__(self) -> None:
        self.pending: dict[str, asyncio.Future[str]] = {}
        self._ack_waiters: dict[str, asyncio.Future] = {}  # ack-type -> future
        # Serializes device prompts: the screen shows one at a time, so
        # concurrent Claude sessions queue (FIFO on this lock) instead of
        # overwriting each other's prompt (which left the loser to time out
        # and fall back to the terminal/phone).
        self._prompt_lock = asyncio.Lock()
        # Serializes ack-style queries (GPIO/ADC/capture). Replies are keyed
        # only by ack-TYPE, so two concurrent queries with the same ack would
        # clobber each other's future; the device is single-threaded anyway.
        self._query_lock = asyncio.Lock()
        self._rx_buf = bytearray()
        self._owner = os.environ.get("BUDDY_OWNER", "CLI")

        self._serial_port = os.environ.get("BUDDY_SERIAL", "").strip()
        self._host   = os.environ.get("BUDDY_HOST", "").strip()
        self._listen = os.environ.get("BUDDY_LISTEN", "").strip()
        self._token  = os.environ.get("BUDDY_TOKEN", "").strip()
        self.transport = ("serial" if self._serial_port
                          else "tcp-listen" if self._listen
                          else "tcp" if self._host else "ble")

        self.client = None          # BleakClient
        self.device = None          # BLEDevice
        self._tcp_reader: Optional[asyncio.StreamReader] = None
        self._tcp_writer: Optional[asyncio.StreamWriter] = None
        self._ser = None            # pyserial Serial

        # Token reporting state (persisted across restarts).
        self._period = os.environ.get("BUDDY_TOKEN_PERIOD", "day")
        self._reset_ts = 0.0

        # Lifetime token tracking: cached total of all-time output tokens from
        # transcripts.  Computed once at startup, then incremented from deltas.
        # _lifetime_baseline is subtracted so "level_reset" can zero the display
        # without losing the underlying history.
        self._lifetime_cached: int = 0          # raw all-time total (from scan)
        self._lifetime_baseline: int = 0        # subtract for displayed value
        self._lifetime_scanned: bool = False    # True after the initial scan
        self._lifetime_scan_mtime: float = 0.0  # newest transcript mtime at last scan

        # Approved/denied tally: list of {"ts": float, "decision": "approved"|"denied"}
        self._decision_log: list[dict] = []

        self._load_state()

    # ── token usage ─────────────────────────────────────────────────

    def _load_state(self) -> None:
        try:
            with STATE_PATH.open() as _fh:
                d = json.load(_fh)
            self._period = d.get("period", self._period)
            self._reset_ts = float(d.get("reset_ts", 0.0))
            self._lifetime_baseline = int(d.get("lifetime_baseline", 0))
            self._decision_log = list(d.get("decision_log", []))
        except Exception:
            pass

    def _save_state(self) -> None:
        try:
            STATE_PATH.write_text(json.dumps({
                "period": self._period,
                "reset_ts": self._reset_ts,
                "lifetime_baseline": self._lifetime_baseline,
                "decision_log": self._decision_log,
            }))
        except Exception:
            pass

    def _scan_all_lifetime_tokens(self) -> int:
        """Sum output_tokens across ALL JSONL transcripts.

        Updates self._lifetime_scan_mtime to the newest file mtime seen so
        heartbeat_loop can skip redundant rescans when nothing has changed.
        """
        total = 0
        max_mtime = 0.0
        try:
            for p in PROJECTS_DIR.glob("*/*.jsonl"):
                try:
                    mtime = p.stat().st_mtime
                    if mtime > max_mtime:
                        max_mtime = mtime
                except OSError:
                    continue
                try:
                    with p.open("r", errors="replace") as f:
                        for line in f:
                            if '"output_tokens"' not in line:
                                continue
                            try:
                                obj = json.loads(line)
                            except Exception:
                                continue
                            usage = (obj.get("message") or {}).get("usage") or {}
                            total += int(usage.get("output_tokens", 0) or 0)
                except OSError:
                    continue
        except Exception:
            pass
        self._lifetime_scan_mtime = max_mtime
        return total

    def _transcripts_changed(self) -> bool:
        """Return True if any transcript has been modified since the last scan.

        Uses a single stat pass over the glob (O(n) but no file I/O): ~2ms on
        157 files.  Called from the heartbeat thread before the ~500ms full scan.
        """
        try:
            for p in PROJECTS_DIR.glob("*/*.jsonl"):
                try:
                    if p.stat().st_mtime > self._lifetime_scan_mtime:
                        return True
                except OSError:
                    continue
        except Exception:
            pass
        return False

    def ensure_lifetime_scanned(self) -> None:
        """Scan all transcripts once at startup to seed the lifetime cache."""
        if self._lifetime_scanned:
            return
        self._lifetime_cached = self._scan_all_lifetime_tokens()
        self._lifetime_scanned = True
        log(f"lifetime token scan complete: {self._lifetime_cached} raw "
            f"(baseline={self._lifetime_baseline}, "
            f"display={self._lifetime_cached - self._lifetime_baseline})")

    def period_tokens(self) -> int:
        """Output tokens across transcripts in the current window, since the
        later of the window start and the last reset."""
        start = max(_period_start(self._period), self._reset_ts)
        total = 0
        try:
            for p in PROJECTS_DIR.glob("*/*.jsonl"):
                try:
                    if p.stat().st_mtime < start - 1:
                        continue   # whole file predates the window
                except OSError:
                    continue
                try:
                    with p.open("r", errors="replace") as f:
                        for line in f:
                            if '"output_tokens"' not in line:
                                continue
                            try:
                                obj = json.loads(line)
                            except Exception:
                                continue
                            ts = obj.get("timestamp")
                            if ts:
                                try:
                                    t = datetime.fromisoformat(
                                        ts.replace("Z", "+00:00")).timestamp()
                                    if t < start:
                                        continue
                                except Exception:
                                    pass
                            usage = (obj.get("message") or {}).get("usage") or {}
                            total += int(usage.get("output_tokens", 0) or 0)
                except OSError:
                    continue
        except Exception:
            pass
        return total

    def lifetime_tokens(self) -> int:
        """Displayed lifetime tokens = raw all-time total minus baseline.

        The raw total is computed once at startup and held in memory; we do NOT
        re-scan every heartbeat.  Any new session tokens are captured the next
        time ensure_lifetime_scanned is called (on reconnect / startup).
        """
        ensure_called = self._lifetime_scanned  # avoid side-effect in property
        if not ensure_called:
            self.ensure_lifetime_scanned()
        displayed = max(0, self._lifetime_cached - self._lifetime_baseline)
        return displayed

    def approved_denied(self) -> tuple[int, int]:
        """Count approved/denied decisions within the current period window."""
        start = max(_period_start(self._period), self._reset_ts)
        ok = 0
        deny = 0
        for entry in self._decision_log:
            if not isinstance(entry, dict):
                continue
            if float(entry.get("ts", 0.0)) < start:
                continue
            decision = entry.get("decision", "")
            if decision == "approved":
                ok += 1
            elif decision == "denied":
                deny += 1
        return ok, deny

    def _record_decision(self, decision: str) -> None:
        """Append a resolved prompt decision to the in-memory log and persist."""
        self._decision_log.append({
            "ts": datetime.now(timezone.utc).timestamp(),
            "decision": decision,
        })
        # Trim very old entries (older than 31 days) to keep the file bounded.
        cutoff = datetime.now(timezone.utc).timestamp() - 32 * 86400
        self._decision_log = [
            e for e in self._decision_log
            if isinstance(e, dict) and float(e.get("ts", 0.0)) >= cutoff
        ]
        self._save_state()

    def token_reset(self) -> None:
        """Zero the period baseline and clear the approved/denied tally."""
        self._reset_ts = datetime.now(timezone.utc).timestamp()
        self._decision_log = []
        self._save_state()
        log("token usage reset (period + approved/denied cleared)")

    def level_reset(self) -> None:
        """Set lifetime_baseline = current raw total so displayed life → 0."""
        self.ensure_lifetime_scanned()
        self._lifetime_baseline = self._lifetime_cached
        self._save_state()
        log(f"level reset: baseline set to {self._lifetime_baseline}")

    def token_period(self, period: str) -> bool:
        if period not in ("day", "week", "month", "all"):
            return False
        self._period = period
        self._save_state()
        log(f"token period -> {period}")
        return True

    # ── transport-agnostic ──────────────────────────────────────────

    def is_connected(self) -> bool:
        if self.transport == "ble":
            return bool(self.client and self.client.is_connected)
        if self.transport == "serial":
            return bool(self._ser and self._ser.is_open)
        return self._tcp_writer is not None and not self._tcp_writer.is_closing()

    @property
    def device_name(self) -> Optional[str]:
        if self.transport == "serial":
            return f"serial:{self._serial_port}"
        if self.transport == "tcp":
            return f"tcp:{self._host}"
        if self.transport == "tcp-listen":
            return f"tcp-listen:{self._listen}"
        return self.device.name if self.device else None

    async def connect(self) -> bool:
        if self.transport == "serial":
            return await self._serial_connect()
        if self.transport == "tcp":
            return await self._tcp_connect()
        if self.transport == "tcp-listen":
            return self.is_connected()
        return await self._ble_connect()

    async def _send_initial(self) -> None:
        now = int(time.time())
        tz = -time.timezone if time.daylight == 0 else -time.altzone
        await self._send_json({"time": [now, tz]})
        await self._send_json({"cmd": "owner", "name": self._owner})
        await self.push_tokens()

    async def _send_json(self, obj: dict) -> None:
        if not self.is_connected():
            return
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            if self.transport == "ble":
                await self.client.write_gatt_char(NUS_RX, line, response=True)
            elif self.transport == "serial":
                self._ser.write(line); self._ser.flush()
            else:
                self._tcp_writer.write(line); await self._tcp_writer.drain()
        except Exception as exc:
            log(f"write failed: {exc}")

    async def push_tokens(self) -> None:
        """Push the full heartbeat token payload to the device."""
        period = self.period_tokens()
        life = self.lifetime_tokens()
        ok, deny = self.approved_denied()
        await self._send_json({
            "cmd": "tokens",
            "set": period,
            "life": life,
            "ok": ok,
            "deny": deny,
        })

    def _feed(self, data: bytes) -> None:
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
                fut.set_result(decision); log(f"device -> {decision} for {pid}")
            return
        if msg.get("cmd") == "status":
            await self._send_json({"ack": "status", "ok": True,
                                   "data": {"name": "claude-code-bridge", "sec": False}})
            return
        # Device → daemon control events (from the on-device Usage menu).
        evt = msg.get("evt")
        if evt == "period":
            value = msg.get("value", "")
            if self.token_period(str(value)):
                log(f"device evt: period -> {value}")
                await self.push_tokens()
            return
        if evt == "token_reset":
            self.token_reset()
            log("device evt: token_reset")
            await self.push_tokens()
            return
        if evt == "level_reset":
            self.level_reset()
            log("device evt: level_reset")
            await self.push_tokens()
            return
        ack = msg.get("ack")
        if ack:
            fut = self._ack_waiters.pop(ack, None)
            if fut and not fut.done():
                fut.set_result(msg)
            return
        # other lines: ignore

    async def query(self, cmd: dict, ack: str, timeout: float = 8.0) -> dict:
        """Send a command and wait for the device's {"ack": <ack>} reply
        (used for GPIO reads / captures that return data)."""
        if not self.is_connected():
            return {"error": "device not connected"}
        async with self._query_lock:
            fut: asyncio.Future = asyncio.get_running_loop().create_future()
            self._ack_waiters[ack] = fut
            await self._send_json(cmd)
            try:
                return await asyncio.wait_for(fut, timeout=timeout)
            except asyncio.TimeoutError:
                self._ack_waiters.pop(ack, None)
                return {"error": "device timeout"}

    # ── BLE ──────────────────────────────────────────────────────────

    async def _ble_connect(self) -> bool:
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError:
            log("BLE transport needs bleak"); return False
        log("scanning for Claude* (5s)…")
        devs = await BleakScanner.discover(timeout=5.0)
        cands = [d for d in devs if (d.name or "").startswith("Claude")]
        if not cands:
            log("no Claude* device found"); return False
        self.device = cands[0]
        self.client = BleakClient(self.device.address)
        try:
            await self.client.connect()
        except Exception as exc:
            log(f"BLE connect failed: {exc}"); return False
        await self.client.start_notify(NUS_TX, lambda _c, d: self._feed(bytes(d)))
        await self._send_initial()
        log(f"BLE linked: {self.device.name}")
        return True

    # ── serial ───────────────────────────────────────────────────────

    async def _serial_connect(self) -> bool:
        try:
            import serial
        except ImportError:
            log("serial transport needs pyserial"); return False
        try:
            ser = serial.Serial()
            ser.port = self._serial_port; ser.baudrate = 115200
            ser.dtr = False; ser.rts = False; ser.timeout = 0
            ser.open()
        except Exception as exc:
            log(f"serial open failed: {exc}"); return False
        self._ser = ser
        await asyncio.sleep(1.2)            # device may reset on open
        try: ser.reset_input_buffer()
        except Exception: pass
        asyncio.create_task(self._serial_read_pump())
        await self._send_initial()
        log(f"serial linked: {self._serial_port}")
        return True

    async def _serial_read_pump(self) -> None:
        while self._ser and self._ser.is_open:
            try:
                n = self._ser.in_waiting
                if n:
                    self._feed(self._ser.read(n))
            except Exception as exc:
                log(f"serial read ended: {exc}"); break
            await asyncio.sleep(0.05)
        try:
            if self._ser: self._ser.close()
        except Exception:
            pass
        self._ser = None

    # ── TCP dial ──────────────────────────────────────────────────────

    async def _tcp_connect(self) -> bool:
        host, _, port_s = self._host.partition(":")
        port = int(port_s or "6400")
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=6)
        except Exception as exc:
            log(f"tcp connect failed: {exc}"); return False
        self._tcp_reader, self._tcp_writer = reader, writer
        if self._token:
            writer.write((self._token + "\n").encode()); await writer.drain()
            try:
                auth = await asyncio.wait_for(reader.readline(), timeout=5)
            except asyncio.TimeoutError:
                log("tcp auth timeout"); await self._tcp_close(); return False
            if b'"ok"' not in auth:
                log("tcp auth rejected"); await self._tcp_close(); return False
        asyncio.create_task(self._tcp_read_pump())
        await self._send_initial()
        log(f"tcp linked: {self._host}")
        return True

    async def _tcp_read_pump(self) -> None:
        try:
            while self._tcp_reader and not self._tcp_reader.at_eof():
                data = await self._tcp_reader.read(512)
                if not data:
                    break
                self._feed(data)
        except Exception as exc:
            log(f"tcp read ended: {exc}")
        await self._tcp_close()

    async def _tcp_close(self) -> None:
        w, self._tcp_writer, self._tcp_reader = self._tcp_writer, None, None
        if w:
            try: w.close()
            except Exception: pass

    async def serve_listener(self) -> None:
        host, _, port_s = self._listen.partition(":")
        port = int(port_s or "6401")

        async def on_device(reader, writer):
            peer = writer.get_extra_info("peername")
            try:
                tok = (await asyncio.wait_for(reader.readline(), timeout=5)).decode().strip()
            except Exception:
                writer.close(); return
            if self._token and tok != self._token:
                log(f"dial-in auth failed from {peer}"); writer.close(); return
            if self.is_connected():
                writer.close(); return
            self._tcp_reader, self._tcp_writer = reader, writer
            log(f"device dialed in from {peer}")
            await self._send_initial()
            try:
                while not reader.at_eof():
                    data = await reader.read(512)
                    if not data: break
                    self._feed(data)
            except Exception as exc:
                log(f"device link ended: {exc}")
            await self._tcp_close()

        server = await asyncio.start_server(on_device, host or "0.0.0.0", port)
        log(f"awaiting device dial-in on {host or '0.0.0.0'}:{port}")
        async with server:
            await server.serve_forever()

    # ── prompt + heartbeat ────────────────────────────────────────────

    async def request_prompt(self, tool, hint, src, timeout_s) -> str:
        if not self.is_connected():
            return "disconnected"
        pid = f"hook_{uuid.uuid4().hex[:10]}"
        fut: asyncio.Future[str] = asyncio.get_running_loop().create_future()
        displayed = False

        async def _show_and_wait() -> str:
            # Only one prompt owns the screen at a time. Concurrent sessions
            # block here (FIFO) and take the device in turn instead of
            # clobbering each other. The id stays unique so the device's
            # decision routes back to the right waiter via self.pending.
            nonlocal displayed
            async with self._prompt_lock:
                self.pending[pid] = fut
                await self._send_json({
                    "total": 1, "running": 0, "waiting": 1, "msg": f"approve: {tool}",
                    "prompt": {"id": pid, "tool": tool, "hint": hint, "src": src},
                })
                displayed = True
                log(f"-> device prompt {pid}: {tool} / {hint!r}")
                return await fut

        try:
            # The timeout covers BOTH queue wait and on-screen wait, so a prompt
            # stuck behind a long one still returns before the hook's socket
            # read gives up (which keeps the calling session unblocked).
            decision = await asyncio.wait_for(_show_and_wait(), timeout=timeout_s)
        except asyncio.TimeoutError:
            self.pending.pop(pid, None)
            if displayed:
                await self._send_json({"total": 0, "running": 0, "waiting": 0, "msg": ""})
                # Device is still showing OUR stale prompt; clear it so the next
                # queued prompt (or idle face) takes over.
                await self._send_json({"cmd": "clearprompt"})
                log(f"clearprompt sent after timeout for {pid}")
            else:
                log(f"prompt {pid} timed out in queue (never shown — screen busy)")
            return "timeout"
        # Record the decision in the approved/denied tally. The device replies
        # with "once"/"always" (approve) or "deny" — normalize before tallying.
        if decision in ("once", "always", "approve", "approved"):
            self._record_decision("approved")
        elif decision in ("deny", "denied", "block"):
            self._record_decision("denied")
        # Resolved — clear the APPROVE screen promptly. promptId is sticky on
        # the firmware now, so a successful decision no longer auto-clears via
        # the next idle frame; without this the approved screen lingers.
        await self._send_json({"cmd": "clearprompt"})
        return decision

    async def heartbeat_loop(self) -> None:
        while True:
            await asyncio.sleep(HEARTBEAT_S)
            if self.is_connected() and not self.pending:
                # Refresh lifetime LIVE each heartbeat so the device's level bar
                # stays current during active sessions.  Skip the ~500ms full scan
                # when no transcript has been written since the last scan (mtime
                # guard costs ~2ms).  Off-thread so the sync scan never blocks
                # the event loop.
                try:
                    changed = await asyncio.to_thread(self._transcripts_changed)
                    if changed:
                        self._lifetime_cached = await asyncio.to_thread(
                            self._scan_all_lifetime_tokens)
                except Exception as exc:
                    log(f"lifetime rescan failed: {exc}")
                await self._send_json({"total": 0, "running": 0, "waiting": 0, "msg": ""})
                await self.push_tokens()

    async def disconnect(self) -> None:
        if self.transport in ("tcp", "tcp-listen"):
            await self._tcp_close()
        elif self.transport == "serial" and self._ser:
            try: self._ser.close()
            except Exception: pass
        elif self.client and self.client.is_connected:
            try: await self.client.disconnect()
            except Exception: pass


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
            period_tokens = link.period_tokens()
            life = link.lifetime_tokens()
            ok, deny = link.approved_denied()
            resp = {
                "connected": link.is_connected(),
                "device": link.device_name,
                "transport": link.transport,
                "periodTokens": period_tokens,
                "lifetimeTokens": life,
                "level": life // 50_000,
                "evoStage": _evo_stage(life),
                "approved": ok,
                "denied": deny,
                "period": link._period,
                # Legacy field kept for backward compatibility.
                "tokens": period_tokens,
            }
            writer.write((json.dumps(resp) + "\n").encode())
        elif op == "prompt":
            decision = await link.request_prompt(
                str(req.get("tool", "?"))[:19], str(req.get("hint", ""))[:43],
                str(req.get("src", "cli"))[:7],
                float(req.get("timeout", DEFAULT_PROMPT_TIMEOUT_S)))
            writer.write((json.dumps({"decision": decision}) + "\n").encode())
        elif op == "send":
            cmd = req.get("cmd")
            ok = isinstance(cmd, dict) and link.is_connected()
            if ok:
                await link._send_json(cmd)
            writer.write((json.dumps({"ok": ok}) + "\n").encode())
        elif op == "query":
            cmd = req.get("cmd"); ack = str(req.get("ack", ""))
            if isinstance(cmd, dict) and ack:
                r = await link.query(cmd, ack, float(req.get("timeout", 8)))
            else:
                r = {"error": "query needs cmd + ack"}
            writer.write((json.dumps(r) + "\n").encode())
        elif op == "clearprompt":
            # Dismiss a stale approval prompt on the device — used by the hook
            # when it falls through to the computer instead of waiting for the
            # device decision (timeout / disconnected / unknown).
            await link._send_json({"cmd": "clearprompt"})
            writer.write((json.dumps({"ok": True}) + "\n").encode())
        elif op == "token":
            action = req.get("action")
            if action == "reset":
                link.token_reset(); await link.push_tokens()
                ok_c, deny_c = link.approved_denied()
                writer.write((json.dumps({
                    "ok": True,
                    "tokens": link.period_tokens(),
                    "approved": ok_c,
                    "denied": deny_c,
                }) + "\n").encode())
            elif action == "period":
                ok_flag = link.token_period(str(req.get("value", "")))
                if ok_flag: await link.push_tokens()
                ok_c, deny_c = link.approved_denied()
                writer.write((json.dumps({
                    "ok": ok_flag,
                    "period": link._period,
                    "tokens": link.period_tokens(),
                    "approved": ok_c,
                    "denied": deny_c,
                }) + "\n").encode())
            elif action == "level_reset":
                link.level_reset(); await link.push_tokens()
                writer.write((json.dumps({
                    "ok": True,
                    "lifetimeTokens": link.lifetime_tokens(),
                    "level": link.lifetime_tokens() // 50_000,
                }) + "\n").encode())
            else:
                writer.write(b'{"error":"bad token action"}\n')
        else:
            writer.write(b'{"error":"unknown op"}\n')
        await writer.drain()
    except Exception as exc:
        log(f"client handler error: {exc}")
    finally:
        writer.close()
        try: await writer.wait_closed()
        except Exception: pass


async def serve_socket(link: BuddyLink) -> None:
    SOCK_DIR.mkdir(parents=True, exist_ok=True)
    if SOCK_PATH.exists():
        SOCK_PATH.unlink()
    server = await asyncio.start_unix_server(
        lambda r, w: handle_client(link, r, w), path=str(SOCK_PATH))
    os.chmod(SOCK_PATH, 0o600)
    log(f"listening on {SOCK_PATH} (transport={link.transport}, period={link._period})")
    async with server:
        await server.serve_forever()


async def main() -> None:
    link = BuddyLink()
    # Seed the lifetime token cache eagerly at startup so the first heartbeat
    # has accurate data.  This is a one-time full scan; subsequent heartbeats
    # use the cached value.
    link.ensure_lifetime_scanned()

    def _shutdown(*_a):
        log("shutdown — closing")
        asyncio.create_task(link.disconnect())
        for t in asyncio.all_tasks():
            t.cancel()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, _shutdown)

    async def reconnector():
        backoff = 2
        while True:
            if not link.is_connected():
                if await link.connect():
                    backoff = 2
                else:
                    await asyncio.sleep(backoff); backoff = min(backoff * 2, 60); continue
            await asyncio.sleep(5)

    tasks = [serve_socket(link), link.heartbeat_loop()]
    tasks.append(link.serve_listener() if link.transport == "tcp-listen" else reconnector())
    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass


if __name__ == "__main__":
    SOCK_DIR.mkdir(parents=True, exist_ok=True)
    asyncio.run(main())
