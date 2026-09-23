"""Run a two-process opensupcom multiplayer check (host + joiner) for CTest.

Both processes talk over localhost TCP and self-report their result through
their exit code. No game data is needed, so these run in CI.

Usage:
    run_pair.py <opensupcom> <mp|lan> <port> [--client-may-fail] [-- extra args...]

Extra args after `--` go to both processes. With --client-may-fail only the
host's exit code decides the result (used when the client deliberately
leaves mid-match, e.g. --mp-drop-at).
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

TIMEOUT_SECONDS = 180


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    exe = Path(argv[0])
    kind, port = argv[1], argv[2]
    rest = argv[3:]
    client_may_fail = "--client-may-fail" in rest
    extra = rest[rest.index("--") + 1 :] if "--" in rest else []

    common = ["--mp-port", port, *extra]
    host = subprocess.Popen([str(exe), f"--{kind}-host", *common])
    # Give the host a moment to bind before the client connects.
    time.sleep(0.5)
    client = subprocess.Popen([str(exe), f"--{kind}-join", "127.0.0.1", *common])

    try:
        client_rc = client.wait(timeout=TIMEOUT_SECONDS)
        host_rc = host.wait(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        for proc in (host, client):
            proc.kill()
        print(f"run_pair: timed out after {TIMEOUT_SECONDS}s", file=sys.stderr)
        return 1

    print(f"run_pair: host exit={host_rc} client exit={client_rc}")
    if host_rc != 0:
        return 1
    if client_rc != 0 and not client_may_fail:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
