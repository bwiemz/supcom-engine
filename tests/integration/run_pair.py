"""Run a two-process opensupcom multiplayer check (host + joiner) for CTest.

Both processes talk over localhost TCP (a lobby, launched into a lockstep
game) and self-report their result through their exit code. No game data is
needed, so these run in CI.

Usage:
    run_pair.py <osc_integration> <port> [--joiner-exe <exe>]
                [--client-may-fail] [-- extra args...]

Extra args after `--` go to both processes. With --client-may-fail only the
host's exit code decides the result (used when the client deliberately
leaves mid-match, e.g. --mp-drop-at).

--joiner-exe runs the joiner from another build (the host's by default):
Windows against Linux (M219). Off Windows, a `.exe` runs under Wine.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

TIMEOUT_SECONDS = 180


def command(exe: Path) -> list[str]:
    """How to run `exe`: a Windows build off Windows, under Wine."""
    if exe.suffix.lower() == ".exe" and os.name != "nt":
        return ["wine", str(exe)]
    return [str(exe)]


def environment() -> dict[str, str]:
    """The processes' environment. Wine, quiet, and with no Mono or Gecko
    to install: a prefix Wine sets up or updates would otherwise wait on a
    prompt no one sees."""
    env = dict(os.environ)
    env.setdefault("WINEDEBUG", "-all")
    env.setdefault("WINEDLLOVERRIDES", "mscoree,mshtml=")
    return env


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe = Path(argv[0])
    port = argv[1]
    rest = argv[2:]
    extra = rest[rest.index("--") + 1 :] if "--" in rest else []
    options = rest[: rest.index("--")] if "--" in rest else rest
    client_may_fail = "--client-may-fail" in options
    joiner_exe = exe
    if "--joiner-exe" in options:
        joiner_exe = Path(options[options.index("--joiner-exe") + 1])

    common = ["--mp-port", port, *extra]
    env = environment()
    host = subprocess.Popen([*command(exe), "--mp-host", *common], env=env)
    # Give the host a moment to listen before the client joins (the joiner
    # retries a refused join, but a refused connect costs Windows seconds).
    time.sleep(0.5)
    client = subprocess.Popen([*command(joiner_exe), "--mp-join", "127.0.0.1", *common], env=env)

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
