"""A recorded game plays back identically (CTest: data.replay_roundtrip).

Plays a game with `--record` (process A), then plays the recording with
`--replay` (process B), each writing `--checksum-trace`. B checks every tick
against the checksums recorded in the replay itself and exits non-zero on the
first difference; the two traces are also compared with tools/checksum_diff.py.
The game should carry player input (`--scripted-orders`), so the replay's
command stream, not only the seed and setup, has to be right.

Usage:
    replay_roundtrip.py <opensupcom> <checksum_diff.py> -- <game args...>

Exits 77 (skipped) when the game has no data to run on.
"""

from __future__ import annotations

import re
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
    if len(argv) < 3 or "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    exe, diff_tool = Path(argv[0]), Path(argv[1])
    game_args = argv[argv.index("--") + 1 :]
    with tempfile.TemporaryDirectory(prefix="osc-replay-") as tmp:
        replay = Path(tmp) / "game.oscreplay"
        trace_a, trace_b = Path(tmp) / "a.txt", Path(tmp) / "b.txt"

        record = run(
            [str(exe), *game_args, "--record", str(replay), "--checksum-trace", str(trace_a)]
        )
        if record.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        if record.returncode != 0:
            print(f"recording run failed: exit {record.returncode}")
            return 1
        written = re.search(r"Replay: (\d+) commands over (\d+) ticks", record.stdout)
        if not written:
            print("the recording run wrote no replay")
            return 1
        commands, ticks = int(written.group(1)), int(written.group(2))
        if commands == 0:
            print("the replay holds no commands: the game had no player input")
            return 1

        play = run([str(exe), "--replay", str(replay), "--checksum-trace", str(trace_b)])
        print(f"recorded {commands} commands over {ticks} ticks")
        for line in play.stdout.splitlines():
            if line.startswith("REPLAY"):
                print(line)
        if play.returncode != 0:
            print(f"playback failed: exit {play.returncode}")
            return 1

        diff = run([sys.executable, str(diff_tool), str(trace_a), str(trace_b)])
        print(diff.stdout, end="")
        return diff.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
