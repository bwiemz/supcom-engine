"""Two runs of the same game must play it the same way (CTest: data.determinism).

Runs `opensupcom <args> --checksum-trace <file>` twice, as separate processes
(so every memory address differs), and compares the per-tick checksum traces
with tools/checksum_diff.py. A game whose outcome depends on hash-map order,
an unseeded or shared random stream, or object addresses fails here, and the
diff names the first tick and the part (rng, armies, entities) that differs.

Usage:
    determinism.py <opensupcom> <checksum_diff.py> -- <game args...>

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
    if len(argv) < 3 or "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    exe, diff_tool = Path(argv[0]), Path(argv[1])
    game_args = argv[argv.index("--") + 1 :]

    with tempfile.TemporaryDirectory(prefix="osc-determinism-") as tmp:
        traces = [Path(tmp) / "a.txt", Path(tmp) / "b.txt"]
        runs = [
            subprocess.Popen(
                [str(exe), *game_args, "--checksum-trace", str(trace)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            for trace in traces
        ]
        codes = [run.wait(timeout=TIMEOUT_SECONDS) for run in runs]
        if SKIPPED in codes:
            print("no game data: skipped")
            return SKIPPED
        if any(codes):
            print(f"a run failed: exit codes {codes}")
            return 1
        diff = subprocess.run(
            [sys.executable, str(diff_tool), *map(str, traces)],
            capture_output=True,
            text=True,
            check=False,
        )
        print(diff.stdout, end="")
        return diff.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
