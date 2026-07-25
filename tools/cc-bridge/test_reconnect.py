#!/usr/bin/env python3
"""Offline tests for the bridge's connect / reconnect / liveness paths.

No hardware, no BLE adapter: each scenario injects a fake `bleak` into
sys.modules and redirects $HOME, so buddy_bridged's Path.home()-derived socket,
log and token state land in a throwaway directory.

    ./test_reconnect.py              # run every scenario, print a summary
    ./test_reconnect.py ble_hang     # run one scenario in-process

Scenarios exist because of a real outage (2026-07-25): the daemon sat dead for
two nights with the process still up and launchd's KeepAlive satisfied.
`_ble_connect` awaited BleakClient.connect() unbounded -- CoreBluetooth's
connectPeripheral: never times out by design -- which parked the reconnector
task, and the reconnector is the only thing that can ever restore the link.

  ble_hang        peripheral accepts the link then stalls the GATT handshake
  ble_ok          healthy peripheral still links (guards the timeout wrap)
  watchdog        an unbounded await with NO inner timeout, i.e. the next
                  regression -- only reconnector()'s own watchdog catches it
  liveness_dead   transport claims .is_connected True but carries no data
  liveness_alive  responsive link must NOT be torn down

Note on $HOME: AF_UNIX paths max out around 104 chars on macOS, so the temp
home has to be short or start_unix_server dies with "AF_UNIX path too long".
The runner uses /tmp for exactly that reason -- don't "fix" it to somewhere
deeper.
"""
import asyncio
import json
import os
import subprocess
import shutil
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = ["ble_hang", "ble_ok", "watchdog", "liveness_dead",
             "liveness_alive", "status_nonblocking"]


# ── fakes ────────────────────────────────────────────────────────────────

class FakeDevice:
    name = "Claude-TEST"
    address = "00:00:00:00:00:00"


def make_scanner(hang: bool = False):
    class _Scanner:
        @staticmethod
        async def discover(timeout=5.0):
            if hang:
                await asyncio.Event().wait()   # unbounded on purpose
            return [FakeDevice()]
    return _Scanner


def make_client(*, hang_connect=False, answers=False, lies=False):
    class _Client:
        def __init__(self, address):
            self.address = address
            # `lies` models bleak reporting a live link for a dead one, which
            # is what made is_connected() untrustworthy during the outage.
            self.is_connected = bool(lies)
            self._cb = None

        async def connect(self):
            if hang_connect:
                await asyncio.Event().wait()
            self.is_connected = True

        async def start_notify(self, char, cb):
            self._cb = cb

        async def write_gatt_char(self, char, data, response=False):
            if not answers or self._cb is None:
                return                      # write "succeeds", nothing returns
            try:
                msg = json.loads(data.decode().strip())
            except Exception:
                return
            if msg.get("cmd") == "status":
                reply = json.dumps({"ack": "status", "ok": True}).encode()
                self._cb(None, bytearray(reply + b"\n"))

        async def disconnect(self):
            if not lies:                    # a lying transport stays "up"
                self.is_connected = False

    return _Client


def install_bleak(client, scanner) -> None:
    mod = types.ModuleType("bleak")
    mod.BleakClient = client
    mod.BleakScanner = scanner
    sys.modules["bleak"] = mod


def load_bridge():
    """Import buddy_bridged only AFTER the fake bleak and $HOME are in place."""
    sys.path.insert(0, HERE)
    for var in ("BUDDY_SERIAL", "BUDDY_HOST", "BUDDY_LISTEN"):
        os.environ.pop(var, None)
    import buddy_bridged
    return buddy_bridged


async def run_daemon(bridge, seconds: float) -> str:
    """Run the real main() for a bounded window; return the log it wrote."""
    bridge.SOCK_DIR.mkdir(parents=True, exist_ok=True)
    if bridge.LOG_PATH.exists():
        bridge.LOG_PATH.unlink()
    try:
        await asyncio.wait_for(bridge.main(), timeout=seconds)
    except (asyncio.TimeoutError, asyncio.CancelledError):
        pass
    return bridge.LOG_PATH.read_text() if bridge.LOG_PATH.exists() else ""


# ── scenarios ────────────────────────────────────────────────────────────

async def s_ble_hang() -> tuple[bool, str]:
    install_bleak(make_client(hang_connect=True, lies=True),
                  make_scanner())
    bridge = load_bridge()
    link = bridge.BuddyLink()
    # getattr, not attribute access: the scenario must be runnable against a
    # build that predates the constant, or it cannot prove it catches the bug.
    bound = getattr(bridge, "BLE_CONNECT_TIMEOUT_S", 20) + 10
    try:
        result = await asyncio.wait_for(link.connect(), timeout=bound)
    except asyncio.TimeoutError:
        return False, (f"connect() still parked after {bound}s -- "
                       f"reconnection is permanently dead")
    if result is not False:
        return False, f"connect() returned {result!r}, expected False"
    return True, "stalled handshake gives up instead of parking"


async def s_ble_ok() -> tuple[bool, str]:
    install_bleak(make_client(answers=True), make_scanner())
    bridge = load_bridge()
    link = bridge.BuddyLink()
    ok = await asyncio.wait_for(link.connect(), timeout=15)
    if ok is not True:
        return False, f"connect() returned {ok!r}, expected True"
    if not link.is_connected():
        return False, "is_connected() False after a successful link"
    if link.client is None:
        return False, "client was cleared on the success path"
    return True, "healthy peripheral still links, client retained"


async def s_watchdog() -> tuple[bool, str]:
    # discover() hangs, so BLE_CONNECT_TIMEOUT_S never applies -- this models
    # the NEXT unbounded await someone adds, not the one already fixed.
    os.environ["BUDDY_CONNECT_WATCHDOG_S"] = "6"
    install_bleak(make_client(), make_scanner(hang=True))
    bridge = load_bridge()
    text = await run_daemon(bridge, 26.0)
    n = text.count("connect attempt aborted")
    if n < 2:
        return False, (f"only {n} aborted-attempt line(s) -- the reconnector "
                       f"parked (outage reproduced)")
    return True, f"watchdog fired {n}x, reconnection stays alive"


async def _liveness(answers: bool) -> tuple[bool, str]:
    install_bleak(make_client(answers=answers, lies=not answers),
                  make_scanner())
    bridge = load_bridge()
    bridge.HEARTBEAT_S = 1               # compress the cadence
    bridge.LIVENESS_PROBE_TIMEOUT_S = 1
    text = await run_daemon(bridge, 14.0)
    torn = text.count("link unresponsive")
    linked = text.count("BLE linked")
    if answers:
        if torn:
            return False, f"healthy link torn down {torn}x -- regression"
        return True, "responsive link left alone"
    if torn < 1:
        return False, "half-dead link never torn down (is_connected() lie wins)"
    if linked < 2:
        return False, f"torn down but never relinked (BLE linked x{linked})"
    return True, f"half-dead link torn down {torn}x and relinked"


async def s_status_nonblocking() -> tuple[bool, str]:
    """An {"op":"status"} must not stall the event loop.

    period_tokens() globs and reads every transcript line-by-line on EVERY
    status call, and with period="all" the mtime filter skips nothing -- so it
    re-reads the whole corpus. Run on the event loop that freezes prompts,
    heartbeats and liveness acks for the duration; observed 2026-07-25 as
    gr0m_status timing out while prompts still worked.
    """
    install_bleak(make_client(answers=True), make_scanner())
    bridge = load_bridge()

    SLOW_S = 2.0
    bridge.BuddyLink.period_tokens = lambda self: (time.sleep(SLOW_S) or 0)

    stop = asyncio.Event()
    ticks = []

    async def monitor():
        # Record BEFORE checking stop: the tick that lands right after a stall
        # is the one that reveals it, and checking stop first drops exactly
        # that sample -- which silently made this scenario unfalsifiable.
        loop = asyncio.get_running_loop()
        while True:
            ticks.append(loop.time())
            if stop.is_set():
                return
            await asyncio.sleep(0.05)

    bridge.SOCK_DIR.mkdir(parents=True, exist_ok=True)
    daemon = asyncio.create_task(bridge.main())
    for _ in range(100):                       # wait for the socket to bind
        if bridge.SOCK_PATH.exists():
            break
        await asyncio.sleep(0.1)
    else:
        daemon.cancel()
        return False, "daemon never bound its socket"

    mon = asyncio.create_task(monitor())
    await asyncio.sleep(0.3)                   # collect a clean baseline
    try:
        reader, writer = await asyncio.open_unix_connection(
            str(bridge.SOCK_PATH))
        t0 = asyncio.get_running_loop().time()
        writer.write(b'{"op":"status"}\n')
        await writer.drain()
        raw = await asyncio.wait_for(reader.readline(), timeout=30)
        call_s = asyncio.get_running_loop().time() - t0
        writer.close()
    finally:
        stop.set()
        await mon
        daemon.cancel()

    # Without these the scenario is unfalsifiable: a handler that errors out
    # closes the connection, readline returns b"", nothing ever stalls, and the
    # test "passes" having measured nothing.
    try:
        reply = json.loads(raw.decode())
    except Exception:
        return False, f"status returned no usable reply ({raw!r})"
    if "transport" not in reply:
        return False, f"status reply missing fields: {reply}"
    if call_s < SLOW_S * 0.9:
        return False, (f"status returned in {call_s:.2f}s -- the slow "
                       f"period_tokens patch never ran, so nothing was tested")

    gaps = [b - a for a, b in zip(ticks, ticks[1:])]
    worst = max(gaps) if gaps else 0.0
    if worst > SLOW_S / 2:
        return False, (f"event loop stalled {worst:.2f}s during one status "
                       f"call -- prompts and acks are frozen that whole time")
    return True, (f"loop stayed responsive (worst gap {worst:.2f}s) while a "
                  f"{call_s:.1f}s status call ran")


async def s_liveness_dead() -> tuple[bool, str]:
    return await _liveness(answers=False)


async def s_liveness_alive() -> tuple[bool, str]:
    return await _liveness(answers=True)


# ── entry points ─────────────────────────────────────────────────────────

async def run_one(name: str) -> int:
    ok, detail = await globals()[f"s_{name}"]()
    print(f"{'PASS' if ok else 'FAIL'}: {name} -- {detail}")
    return 0 if ok else 1


def run_all() -> int:
    failures = []
    for name in SCENARIOS:
        # Short $HOME: AF_UNIX paths are capped near 104 chars.
        home = tempfile.mkdtemp(prefix="cb", dir="/tmp")
        os.makedirs(os.path.join(home, ".claude", "projects"), exist_ok=True)
        env = dict(os.environ, HOME=home)
        env.pop("BUDDY_CONNECT_WATCHDOG_S", None)
        try:
            proc = subprocess.run([sys.executable, os.path.abspath(__file__),
                                   name],
                                  env=env, capture_output=True, text=True,
                                  timeout=120)
            line = [ln for ln in proc.stdout.splitlines()
                    if ln.startswith(("PASS:", "FAIL:"))]
            print(line[-1] if line else
                  f"FAIL: {name} -- no verdict\n{proc.stdout}{proc.stderr}")
            if proc.returncode != 0:
                failures.append(name)
        except subprocess.TimeoutExpired:
            print(f"FAIL: {name} -- scenario timed out")
            failures.append(name)
        finally:
            shutil.rmtree(home, ignore_errors=True)
    print()
    if failures:
        print(f"{len(failures)}/{len(SCENARIOS)} failed: {', '.join(failures)}")
        return 1
    print(f"all {len(SCENARIOS)} scenarios passed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1:
        if sys.argv[1] not in SCENARIOS:
            sys.exit(f"unknown scenario {sys.argv[1]!r}; "
                     f"pick from {', '.join(SCENARIOS)}")
        sys.exit(asyncio.run(run_one(sys.argv[1])))
    sys.exit(run_all())
