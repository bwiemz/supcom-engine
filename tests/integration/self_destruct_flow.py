"""A windowed game whose player is ARMY_2 replays headless.

CTest: data.self_destruct_flow. Runs `--self-destruct-flow-test` in a
temporary user folder: a skirmish launched from the front end with an AI in
the first slot and the player in the second, played offscreen; the player
self-destructs their commander as Ctrl-K does. The game's UI focuses ARMY_2;
its recording (LastGame) then plays headless with `--replay`, where nothing
focuses it, and must match the recording at every beat. Whatever the UI alone
decides must not reach the sim's checksum. On a divergence, both runs'
checksum traces are compared to name the domains that differ.

Usage:
    self_destruct_flow.py <opensupcom> <checksum_diff.py>

Exits 77 (skipped) when the game has no data.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 900
SKIPPED = 77


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False
    )


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe, diff_tool = argv
    with tempfile.TemporaryDirectory(prefix="osc-self-destruct-flow-") as tmp:
        user_dir = Path(tmp)
        live_trace, replay_trace = user_dir / "live.txt", user_dir / "replay.txt"
        game = run(
            [
                exe,
                "--user-dir",
                str(user_dir),
                "--self-destruct-flow-test",
                "--checksum-trace",
                str(live_trace),
            ]
        )
        if game.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        for line in game.stdout.splitlines():
            if "self-destruct-flow" in line:
                print(line)
        replays = sorted(user_dir.glob("replays/*/*.oscreplay"))
        if game.returncode != 0 or not replays:
            print(f"the game failed: exit {game.returncode}, replays {replays}")
            return 1

        replay = run([exe, "--replay", str(replays[0]), "--checksum-trace", str(replay_trace)])
        for line in replay.stdout.splitlines():
            if line.startswith("REPLAY"):
                print(line)
        if replay.returncode != 0:
            diff = run([sys.executable, diff_tool, str(live_trace), str(replay_trace)])
            print(diff.stdout, end="")
        return replay.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
