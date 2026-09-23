"""A recorded game opens in the game through retail's replay dialog path.

CTest: data.replay_flow. Records a short game (`--record`) into a temporary
user folder, as `replays/Player/flow.oscreplay`, then runs
`--user-dir <folder> --replay-flow-test`. The engine lists the folder with
GetSpecialFiles, opens the replay with LaunchReplaySession, as retail's replay
dialog does, and watches it to its end in the game, offscreen. It checks every
tick against the recording and that the UI knows it is a replay.

Usage:
    replay_flow.py <opensupcom> -- <game args for the recording...>

Exits 77 (skipped) when the game has no data to run on.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 900
SKIPPED = 77


def main(argv: list[str]) -> int:
    if len(argv) < 2 or "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    exe = Path(argv[0])
    game_args = argv[argv.index("--") + 1 :]
    with tempfile.TemporaryDirectory(prefix="osc-replay-flow-") as tmp:
        user_dir = Path(tmp)
        replay = user_dir / "replays" / "Player" / "flow.oscreplay"
        record = subprocess.run(
            [str(exe), *game_args, "--record", str(replay)],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        if record.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        if record.returncode != 0 or not replay.exists():
            print(f"recording failed: exit {record.returncode}")
            return 1

        watch = subprocess.run(
            [str(exe), "--user-dir", str(user_dir), "--replay-flow-test"],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        for line in watch.stdout.splitlines():
            if "replay-flow" in line:
                print(line)
        return watch.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
