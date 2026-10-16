"""A network game saved and resumed together (OE-128), by two processes.

CTest: data.lan_game_resume. Two runs of retail's LAN lobby (lan_game.py's
flow), the two players keeping their user folders between them:

  1. The host saves the game in play (--lan-game-save), through the in-game
     save: every player writes their own copy. Both play on to tick 150 and
     log their checksum there.
  2. The host's lobby resumes that save (--lan-game-resume): each player
     launches from their own copy, and both play in lockstep from the saved
     tick to tick 150.

The players give no orders, so the resumed game must arrive where the saved
one did: all four checksums at tick 150 the same. And each player must play
the army they played before, though the second lobby draws spawns again.

Usage:
    lan_game_resume.py <opensupcom> <port>

Exits 77 (skipped) when the game has no data to run on.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

from lan_game import SKIPPED, report, wait_for

SAVE = "resumetest"
CHECKSUM = re.compile(r"\[lan-game\] checksum at tick (\d+): ([0-9a-f]{8})")
ARMY = re.compile(r"\[PASS\] lan-game \(\w+\):.* as army (\d+)")


def run(exe: str, port: str, root: Path, label: str, extra: list[str]) -> dict[str, str] | int:
    """One lobby game; each side's checksum line, or an exit code."""
    roles = {"host": ["--lan-game-host"], "joiner": ["--lan-game-join", "127.0.0.1"]}
    procs: dict[str, subprocess.Popen[bytes]] = {}
    logs: dict[str, Path] = {}
    for name, role in roles.items():
        user_dir = root / name
        user_dir.mkdir(exist_ok=True)
        logs[name] = root / f"{label}-{name}.log"
        with logs[name].open("wb") as out:
            procs[name] = subprocess.Popen(
                [exe, "--user-dir", str(user_dir), *role, "--mp-port", port, *extra],
                stdout=out,
                stderr=subprocess.STDOUT,
            )
    codes = wait_for(procs)
    if all(code == SKIPPED for code in codes.values()):
        return SKIPPED
    print(f"--- {label} ---")
    for name in roles:
        report(name, logs[name], codes.get(name))
    if any(code != 0 for code in codes.values()):
        return 1
    sums: dict[str, str] = {}
    for name in roles:
        text = logs[name].read_text(errors="replace")
        found = CHECKSUM.findall(text)
        army = ARMY.findall(text)
        if found and army:
            sums[name] = f"tick {found[-1][0]}: {found[-1][1]}, army {army[-1]}"
    return sums


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe, port = argv[0], argv[1]
    with tempfile.TemporaryDirectory(prefix="osc-lan-resume-") as tmp:
        root = Path(tmp)
        saved = run(exe, port, root, "saved", ["--lan-game-save", SAVE])
        if isinstance(saved, int):
            if saved == SKIPPED:
                print("no game data: skipped")
            return saved
        resumed = run(exe, port, root, "resumed", ["--lan-game-resume", SAVE])
        if isinstance(resumed, int):
            return resumed
        print(f"saved run: {saved}")
        print(f"resumed run: {resumed}")
        if saved != resumed or len(saved) != 2:
            print("FAIL: the resumed game isn't the saved one, or a player changed army")
            return 1
        sums = {v.split(", army")[0] for v in saved.values()}
        if len(sums) != 1:
            print("FAIL: the players' games differ")
            return 1
        print(f"PASS: resumed in step, each in their own army, the same game at {sums.pop()}")
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
