"""Retail's LAN lobby, played to a game by two processes (M218c).

CTest: data.lan_game. One process hosts retail's lobby (lobby.lua) on
<port>; the other joins it once it listens. Each readies its slot, the host
presses Launch, retail counts down, and the game each launches plays in
lockstep over the lobby's connections to tick 100 with no desync, each
process as its own slot's army. Offscreen, each in a user folder of its
own; any script error fails it.

Usage:
    lan_game.py <opensupcom> <port> [-- extra args for both...]

Exits 77 (skipped) when the game has no data to run on.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 600
SKIPPED = 77


def report(name: str, log: Path, code: int | None) -> None:
    print(f"{name}: exit {code}")
    lines = log.read_text(errors="replace").splitlines()
    marked = [line for line in lines if "[PASS]" in line or "[FAIL]" in line]
    for line in marked:
        print(f"  {line}")
    if code != 0:
        print(f"  --- the last lines of its log ---")
        for line in lines[-40:]:
            print(f"  {line}")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe, port = argv[0], argv[1]
    extra = argv[argv.index("--") + 1 :] if "--" in argv else []
    with tempfile.TemporaryDirectory(prefix="osc-lan-game-") as tmp:
        root = Path(tmp)
        runs = {
            "host": ["--lan-game-host"],
            "joiner": ["--lan-game-join", "127.0.0.1"],
        }
        procs: dict[str, subprocess.Popen[bytes]] = {}
        logs: dict[str, Path] = {}
        for name, role in runs.items():
            user_dir = root / name
            user_dir.mkdir()
            logs[name] = root / f"{name}.log"
            with logs[name].open("wb") as out:
                procs[name] = subprocess.Popen(
                    [exe, "--user-dir", str(user_dir), *role, "--mp-port", port, *extra],
                    stdout=out,
                    stderr=subprocess.STDOUT,
                )
        codes: dict[str, int | None] = {}
        try:
            for name, proc in procs.items():
                codes[name] = proc.wait(timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            print(f"lan_game: timed out after {TIMEOUT_SECONDS}s")
            for name, proc in procs.items():
                proc.kill()
                codes[name] = proc.wait()
        if all(code == SKIPPED for code in codes.values()):
            print("no game data: skipped")
            return SKIPPED
        for name in runs:
            report(name, logs[name], codes.get(name))
        return 0 if all(code == 0 for code in codes.values()) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
