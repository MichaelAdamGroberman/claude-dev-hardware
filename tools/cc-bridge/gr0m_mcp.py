#!/usr/bin/env python3
"""
gr0m-mcp — a tiny dependency-free MCP server (stdio, newline-delimited
JSON-RPC 2.0) that lets Claude control/query the gr0m Hardware Buddy.

It is a thin client of the buddy_bridged daemon's Unix socket
(~/.cache/claude-buddy/buddy.sock), so it works over whatever transport the
daemon is using (serial / BLE / WiFi). No third-party packages required.

Tools:
  gr0m_status                       battery/mode/uptime + token usage
  gr0m_notify(message)              show a line on the device screen
  gr0m_set_radio(mode)              wifi | bt | off  (reboots the device)
  gr0m_set_owner(name)              on-screen owner name
  gr0m_token_reset()                zero the usage counter
  gr0m_token_period(period)         day | week | month | all
"""
import json
import socket
import sys
from pathlib import Path

SOCK_PATH = Path.home() / ".cache" / "claude-buddy" / "buddy.sock"
PROTOCOL_VERSION = "2024-11-05"

TOOLS = [
    {"name": "gr0m_status", "description": "Get the gr0m device status: connection, transport, battery, and token usage for the current period.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "gr0m_notify", "description": "Show a short message on the gr0m device screen.",
     "inputSchema": {"type": "object", "properties": {"message": {"type": "string"}}, "required": ["message"]}},
    {"name": "gr0m_set_radio", "description": "Set the device radio mode (mutually exclusive). Reboots the device.",
     "inputSchema": {"type": "object", "properties": {"mode": {"type": "string", "enum": ["wifi", "bt", "off"]}}, "required": ["mode"]}},
    {"name": "gr0m_set_owner", "description": "Set the on-screen owner name shown on the device.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}},
    {"name": "gr0m_token_reset", "description": "Reset (zero) the device's token-usage counter for the current period.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "gr0m_token_period", "description": "Set the token-usage reporting window shown on the device.",
     "inputSchema": {"type": "object", "properties": {"period": {"type": "string", "enum": ["day", "week", "month", "all"]}}, "required": ["period"]}},
]


def _daemon(req: dict, timeout: float = 35.0) -> dict:
    """Send one request line to the daemon socket, return the JSON reply."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(str(SOCK_PATH))
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        s.close()
        return json.loads(buf.decode().splitlines()[0]) if buf else {}
    except (FileNotFoundError, ConnectionRefusedError, socket.error):
        return {"error": "buddy daemon not running"}
    except Exception as exc:
        return {"error": str(exc)}


def _call_tool(name: str, args: dict) -> str:
    if name == "gr0m_status":
        r = _daemon({"op": "status"})
        return json.dumps(r)
    if name == "gr0m_notify":
        r = _daemon({"op": "send", "cmd": {"total": 0, "running": 0, "waiting": 0,
                                           "msg": str(args.get("message", ""))[:60]}})
        return "shown on device" if r.get("ok") else json.dumps(r)
    if name == "gr0m_set_radio":
        r = _daemon({"op": "send", "cmd": {"cmd": "radio", "mode": args.get("mode", "off")}})
        return f"radio -> {args.get('mode')} (device rebooting)" if r.get("ok") else json.dumps(r)
    if name == "gr0m_set_owner":
        r = _daemon({"op": "send", "cmd": {"cmd": "owner", "name": str(args.get("name", ""))[:12]}})
        return "owner set" if r.get("ok") else json.dumps(r)
    if name == "gr0m_token_reset":
        return json.dumps(_daemon({"op": "token", "action": "reset"}))
    if name == "gr0m_token_period":
        return json.dumps(_daemon({"op": "token", "action": "period", "value": args.get("period", "day")}))
    return f"unknown tool: {name}"


def _send(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main() -> None:
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except Exception:
            continue
        method = msg.get("method")
        mid = msg.get("id")
        if method == "initialize":
            _send({"jsonrpc": "2.0", "id": mid, "result": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "gr0m", "version": "1.0.0"}}})
        elif method == "tools/list":
            _send({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            params = msg.get("params") or {}
            text = _call_tool(params.get("name", ""), params.get("arguments") or {})
            _send({"jsonrpc": "2.0", "id": mid,
                   "result": {"content": [{"type": "text", "text": text}]}})
        elif method == "ping":
            _send({"jsonrpc": "2.0", "id": mid, "result": {}})
        elif method and method.startswith("notifications/"):
            pass  # notifications carry no id and need no response
        elif mid is not None:
            _send({"jsonrpc": "2.0", "id": mid,
                   "error": {"code": -32601, "message": f"method not found: {method}"}})


if __name__ == "__main__":
    main()
