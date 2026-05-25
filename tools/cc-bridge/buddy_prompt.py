#!/usr/bin/env python3
"""
buddy-prompt — Claude Code PreToolUse hook that asks the Hardware
Buddy device to approve or deny a tool call.

Claude Code hook contract:
  - stdin: JSON describing the tool call ({tool_name, tool_input, …})
  - exit 0 with no stdout = pass through (Claude shows its own prompt)
  - stdout JSON with {"decision":"approve"|"block","reason":"…"}
    overrides the default and tells Claude what to do

We talk to buddy_bridged.py over its Unix socket, wait for the user to
press A (approve) or B (deny) on the device, and translate that into the
hook's response format.

Behavior:
  - device approves → {"decision":"approve"} (Claude runs the tool)
  - device denies  → {"decision":"block","reason":"denied on Hardware Buddy"}
  - timeout / no device → exit 0 with no output (Claude falls back to its
    own confirmation prompt, so you're never locked out if the device
    is offline)
"""
from __future__ import annotations

import fnmatch
import json
import socket
import sys
from pathlib import Path

SOCK_PATH = Path.home() / ".cache" / "claude-buddy" / "buddy.sock"
# How long the device has to react. Default matches the firmware's own
# 30s "approve?" timer. Override per-tool by setting BUDDY_TIMEOUT in the
# hook environment.
DEFAULT_TIMEOUT_S = 30


def _clearprompt_device() -> None:
    """Best-effort: tell the daemon to clear the stale approval prompt from
    the device screen.  Silently swallows all errors so call sites stay clean."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(str(SOCK_PATH))
        try:
            s.sendall((json.dumps({"op": "clearprompt"}) + "\n").encode("utf-8"))
            # Read and discard the {"ok":true} reply so the daemon can flush.
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(256)
                if not chunk:
                    break
                buf += chunk
        finally:
            s.close()
    except Exception:
        pass


def _show_message(text: str) -> None:
    """Best-effort: display a one-line message on the device screen WITHOUT an
    approve/deny prompt. Used for tools the device's two buttons can't answer —
    e.g. AskUserQuestion, whose 2-4 option choice happens in the terminal.
    Mirrors gr0m_notify's wire format ({total,running,waiting,msg}). Swallows
    all errors so the question still proceeds if the device is offline."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(str(SOCK_PATH))
        try:
            cmd = {"total": 0, "running": 0, "waiting": 0, "msg": text[:60]}
            s.sendall((json.dumps({"op": "send", "cmd": cmd}) + "\n").encode("utf-8"))
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(256)
                if not chunk:
                    break
                buf += chunk
        finally:
            s.close()
    except Exception:
        pass


def _allow_rules(cwd: str):
    """permissions.allow entries from user + project settings (best-effort)."""
    rules = []
    paths = [Path.home() / ".claude" / "settings.json"]
    if cwd:
        paths += [Path(cwd) / ".claude" / "settings.local.json",
                  Path(cwd) / ".claude" / "settings.json"]
    for p in paths:
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
            rules += ((d.get("permissions") or {}).get("allow") or [])
        except Exception:
            pass
    return rules


def _auto_approved(tool_name: str, tool_input: dict, rules) -> bool:
    """True if an allow rule means Claude runs this WITHOUT prompting — so the
    device shouldn't show a phantom approval with no on-screen counterpart.
    Best-effort match of Claude Code's permission syntax; a false match only
    costs a missed device prompt (Claude still gates on its side)."""
    cmd = tool_input.get("command", "") or ""
    fpath = tool_input.get("file_path", "") or tool_input.get("path", "") or ""
    for r in rules:
        if r == tool_name:
            return True                                  # bare tool: all uses allowed
        if r.startswith(tool_name + "(") and r.endswith(")"):
            inner = r[len(tool_name) + 1:-1].strip()
            if inner in ("*", ""):
                return True
            if tool_name == "Bash":
                spec = inner[:-2] if inner.endswith(":*") else inner
                if spec.endswith("*"):
                    spec = spec[:-1]
                if cmd == inner or (spec and cmd.startswith(spec)) or fnmatch.fnmatch(cmd, inner):
                    return True
            elif fpath == inner or fnmatch.fnmatch(fpath, inner):
                return True
    return False


def main() -> int:
    # Read the hook payload from stdin.
    try:
        payload = json.load(sys.stdin)
    except Exception:
        # No payload, nothing to gate — pass through silently.
        return 0

    tool_name = payload.get("tool_name", "?")
    tool_input = payload.get("tool_input", {}) or {}

    # AskUserQuestion is a 2-4 option question, not a binary approve/deny, so the
    # device's A/B buttons can't answer it. Mirror the question text to the screen
    # (display-only) and pass through, leaving the actual choice to Claude's
    # terminal picker. Not gated by permission_mode — questions always prompt.
    if tool_name == "AskUserQuestion":
        qs = tool_input.get("questions") or []
        if qs and isinstance(qs[0], dict):
            _show_message(str(qs[0].get("question", "") or qs[0].get("header", "")))
        return 0

    # Only wake the device when the user's approval is actually required. In
    # modes that auto-run the tool there is nothing to decide, so pass through
    # silently (no device prompt). Check both field spellings to be safe, and
    # log what Claude Code actually sends so the gating can be verified.
    mode = payload.get("permission_mode") or payload.get("permissionMode") or ""
    try:
        with (Path.home() / ".cache" / "claude-buddy" / "hook-debug.log").open("a") as _f:
            _f.write(f"{tool_name}\tmode={mode!r}\n")
    except Exception:
        pass
    EDIT_TOOLS = ("Edit", "Write", "MultiEdit", "NotebookEdit")
    if mode in ("bypassPermissions", "plan"):
        return 0                                   # nothing requires approval
    if mode == "acceptEdits" and tool_name in EDIT_TOOLS:
        return 0                                   # edits auto-accepted

    # Auto-approved by an allow rule → Claude runs it without prompting, so the
    # device shouldn't show a phantom approval that has no on-screen counterpart.
    if _auto_approved(tool_name, tool_input, _allow_rules(payload.get("cwd", ""))):
        return 0

    # Build a short hint string. Bash commands are the most important
    # case so prefer the command itself; otherwise pick a meaningful
    # input field if we recognize one.
    if tool_name == "Bash":
        hint = tool_input.get("command", "")
    elif tool_name in ("Read", "Edit", "Write"):
        hint = tool_input.get("file_path", "") or tool_input.get("path", "")
    else:
        # Generic fallback — first string-ish value in tool_input.
        hint = ""
        for v in tool_input.values():
            if isinstance(v, str):
                hint = v
                break

    # Open socket.
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(DEFAULT_TIMEOUT_S + 5)
        sock.connect(str(SOCK_PATH))
    except (FileNotFoundError, ConnectionRefusedError, socket.error):
        # No daemon running — fall back to Claude's own prompt.
        return 0

    req = {
        "op": "prompt",
        "tool": tool_name,
        "hint": hint,
        "src": "cli",
        "timeout": DEFAULT_TIMEOUT_S,
    }
    try:
        sock.sendall((json.dumps(req) + "\n").encode("utf-8"))
        # Read one line of reply.
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        reply = json.loads(buf.decode("utf-8").splitlines()[0])
    except Exception:
        # The prompt was already sent to the device; clear it so the device
        # doesn't leave a stale approval screen while Claude prompts on-screen.
        _clearprompt_device()
        return 0
    finally:
        sock.close()

    decision = reply.get("decision", "")
    if decision in ("once", "always"):
        # "always" can't install a permanent rule from a hook, so it behaves
        # like "once" here (allow this call) instead of falling through to a
        # second terminal prompt.
        # Current Claude Code PreToolUse contract: hookSpecificOutput /
        # permissionDecision. The old top-level {"decision":"approve"} is
        # DEPRECATED and silently ignored on recent versions — which is why
        # pressing A on the device registered but never auto-accepted the
        # session's tool call.
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow",
        }}))
        return 0
    if decision == "deny":
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": "denied on Hardware Buddy",
        }}))
        return 0
    # timeout / disconnected / unknown → fall through to Claude's prompt.
    # The device is still showing the approval screen; clear it now so the
    # device doesn't display a stale prompt while the computer handles it.
    _clearprompt_device()
    return 0


if __name__ == "__main__":
    sys.exit(main())
