"""Windows and Linux in one game (M219): the lockstep pairs across builds.

Each of the data-free two-process pairs (run_pair.py: in sync, a divergence
caught, a vanished joiner dropped, a slow joiner setting the pace) is played
twice: a Linux build hosting a Windows build, then the other way round. The
Windows build runs under Wine off Windows. Both sides' sims must agree tick
for tick, so this checks the wire and the sim's determinism across the two
compilers and C runtimes.

Usage:
    cross_os_pairs.py <linux osc_integration> <windows osc_integration.exe>
                      [--first-port N]

Exit 0 when every pair passes.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import run_pair

# The pairs CTest runs (tests/integration/CMakeLists.txt), by their options.
PAIRS: list[tuple[str, list[str]]] = [
    ("lockstep_sync", []),
    ("lockstep_desync_detected", ["--", "--mp-desync"]),
    ("lockstep_peer_drop", ["--client-may-fail", "--", "--mp-drop-at", "20"]),
    ("lockstep_slow_peer", ["--", "--mp-slow", "200", "--mp-frames", "80"]),
]


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    builds = {"linux": Path(argv[0]), "windows": Path(argv[1])}
    port = int(argv[argv.index("--first-port") + 1]) if "--first-port" in argv else 29761
    failed: list[str] = []
    for host, joiner in (("linux", "windows"), ("windows", "linux")):
        for name, options in PAIRS:
            label = f"{name}: {host} hosts {joiner}"
            print(f"=== {label}", flush=True)
            start = time.monotonic()
            code = run_pair.main(
                [str(builds[host]), str(port), "--joiner-exe", str(builds[joiner]), *options]
            )
            print(
                f"=== {label}: {'pass' if code == 0 else 'FAIL'} "
                f"({time.monotonic() - start:.1f} s)",
                flush=True,
            )
            if code != 0:
                failed.append(label)
            port += 1
    if failed:
        print("failed:\n  " + "\n  ".join(failed))
        return 1
    print(f"all {2 * len(PAIRS)} cross-OS pairs passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
