#!/usr/bin/env python3
"""Client for the CombatMod dev bridge (127.0.0.1:6400).

  python bridge_client.py status
  python bridge_client.py read 7FF612340000 64
  python bridge_client.py --port 6401 chain 7FF6A0001000 0 CC0
  python bridge_client.py -m "ping" "selftest" "hooks"   # several commands on one connection
  python bridge_client.py                                # interactive prompt

As a module:
  from bridge_client import send, send_many, ok
  print(send("timecurve"))
  replies = send_many(["trace rva:1234", "tracelog rva:1234 5", "untrace rva:1234"])
  assert all(ok(r) for r in replies)

A reply is the text of all lines; its last line is "OK" or "ERR <msg>".
"""
import os
import socket
import sys

HOST, PORT = "127.0.0.1", 6400
LIVE_PORT = 6400   # the real game's bridge: only reachable on purpose (COMBATMOD_LIVE=1), so a test script can never hit the running game by accident


def _guard(port):
    if port == LIVE_PORT and os.environ.get("COMBATMOD_LIVE") != "1":
        raise SystemExit("bridge_client: refusing to talk to port 6400 (the REAL game). Test hosts use port 6401. Set COMBATMOD_LIVE=1 to talk to the game on purpose.")


def _timeout_for(cmd, default):
    """Long-running commands get a longer socket timeout."""
    c = cmd.strip().split(None, 1)[0].lower() if cmd.strip() else ""
    if c == "keys":
        return max(default, 70.0)  # scripts run up to 60 s
    if c in ("selftest", "rescan", "call"):
        return max(default, 20.0)
    if c == "launch" and cmd.strip().lower().split()[1:2] == ["scan"]:
        return max(default, 120.0)  # the whole-process scan has a 25 s budget per call
    return default


def _read_reply(f):
    lines = []
    for raw in f:
        line = raw.rstrip("\r\n")
        lines.append(line)
        if line == "OK" or line == "ERR" or line.startswith("ERR "):
            return "\n".join(lines)
    raise ConnectionError("bridge closed the connection before a final OK/ERR line (got: %r)" % lines[-3:])


def ok(reply):
    """True if the reply's final line is OK."""
    return reply.rstrip("\n").rsplit("\n", 1)[-1] == "OK"


def send(cmd, host=HOST, port=PORT, timeout=5.0):
    """Send one command line; return the full reply (last line is 'OK' or 'ERR ...')."""
    _guard(port)
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.settimeout(_timeout_for(cmd, timeout))
        s.sendall((cmd.strip() + "\n").encode("utf-8"))
        return _read_reply(s.makefile("r", encoding="utf-8", errors="replace", newline="\n"))


def send_many(cmds, host=HOST, port=PORT, timeout=5.0):
    """Send several commands over ONE connection, in order. Returns a list of replies (one per command)."""
    _guard(port)
    out = []
    with socket.create_connection((host, port), timeout=timeout) as s:
        f = s.makefile("r", encoding="utf-8", errors="replace", newline="\n")
        for cmd in cmds:
            s.settimeout(_timeout_for(cmd, timeout))
            s.sendall((cmd.strip() + "\n").encode("utf-8"))
            out.append(_read_reply(f))
    return out


def main(argv):
    port = PORT
    if len(argv) >= 2 and argv[0] == "--port":
        port, argv = int(argv[1]), argv[2:]
    if argv and argv[0] == "-m":
        for cmd, rep in zip(argv[1:], send_many(argv[1:], port=port)):
            print("> " + cmd)
            print(rep)
        return 0
    if argv:
        print(send(" ".join(argv), port=port))
        return 0
    try:
        while True:
            cmd = input("bridge> ").strip()
            if cmd in ("exit", "quit"):
                break
            if cmd:
                print(send(cmd, port=port))
    except (EOFError, KeyboardInterrupt):
        pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except OSError as e:
        print("ERR cannot reach bridge: %s" % e, file=sys.stderr)
        sys.exit(1)
