"""A saved game loads and plays on as the game did (CTest: data.save_load).

Three processes, each writing `--checksum-trace`:

  A  plays the game and saves it after tick SAVE_1 (`--save-at`).
  B  loads A's save (`--load`), catches up, plays on, and saves again after
     tick SAVE_2: a save made in a loaded game.
  C  loads B's save and plays on.

With `--scripted-orders`, each save is made with one of the player's orders
still to run, which only the save carries. B checks every tick it catches up
against the checksums in A's save, and C against B's; either exits non-zero
on the first difference. Then B's trace must match A's up to SAVE_2 (after
it, B gave an order A never did), and C's must match B's throughout: domain
by domain, with tools/checksum_diff.py.

Usage:
    save_load.py <opensupcom> <checksum_diff.py> -- <game args...>

The game args must include `--ticks` past SAVE_2. Exits 77 (skipped) when
the game has no data to run on.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 900
SKIPPED = 77
SAVE_1 = 600
SAVE_2 = 900


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False
    )


def load_args(game_args: list[str]) -> list[str]:
    """The game args a load keeps: the run's length and the player's
    scripted orders. The map, armies and seed come from the save."""
    kept: list[str] = []
    i = 0
    while i < len(game_args):
        arg = game_args[i]
        if arg == "--ticks":
            kept += game_args[i : i + 2]
            i += 2
            continue
        if arg in ("--ai-skirmish", "--scripted-orders"):
            kept.append(arg)
        elif arg in ("--map", "--ai-armies", "--seed"):
            i += 1  # and its value
        i += 1
    return kept


def head(trace: Path, last_tick: int, out: Path) -> Path:
    """`trace` up to and including `last_tick`."""
    lines: list[str] = []
    for line in trace.read_text().splitlines():
        if line.startswith("#") or int(line.split()[0]) <= last_tick:
            lines.append(line)
    _ = out.write_text("\n".join(lines) + "\n")
    return out


def compare(diff_tool: Path, a: Path, b: Path, what: str) -> bool:
    diff = run([sys.executable, str(diff_tool), str(a), str(b)])
    print(f"{what}: {diff.stdout.strip()}")
    return diff.returncode == 0


def loaded(proc: subprocess.CompletedProcess[str], name: str, tick: int) -> bool:
    lines = [line for line in proc.stdout.splitlines() if line.startswith("LOAD")]
    for line in lines:
        print(f"{name}: {line}")
    if proc.returncode != 0:
        print(f"{name} failed: exit {proc.returncode}")
        return False
    if f"LOAD resumed tick={tick}" not in lines:
        print(f"{name} never took over the game at tick {tick}")
        return False
    return True


def main(argv: list[str]) -> int:
    if len(argv) < 3 or "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    exe, diff_tool = Path(argv[0]), Path(argv[1])
    game_args = argv[argv.index("--") + 1 :]
    with tempfile.TemporaryDirectory(prefix="osc-save-load-") as tmp:
        d = Path(tmp)
        save_1, save_2 = d / "one.oscsave", d / "two.oscsave"
        trace = {name: d / f"{name}.txt" for name in "abc"}

        a = run(
            [
                str(exe),
                *game_args,
                "--save",
                str(save_1),
                "--save-at",
                str(SAVE_1),
                "--checksum-trace",
                str(trace["a"]),
            ]
        )
        if a.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        if a.returncode != 0 or not save_1.exists():
            print(f"the game run failed or saved nothing: exit {a.returncode}")
            return 1

        b = run(
            [
                str(exe),
                "--load",
                str(save_1),
                *load_args(game_args),
                "--save",
                str(save_2),
                "--save-at",
                str(SAVE_2),
                "--checksum-trace",
                str(trace["b"]),
            ]
        )
        if not loaded(b, "B", SAVE_1) or not save_2.exists():
            return 1
        c = run(
            [
                str(exe),
                "--load",
                str(save_2),
                *load_args(game_args),
                "--checksum-trace",
                str(trace["c"]),
            ]
        )
        if not loaded(c, "C", SAVE_2):
            return 1

        same = compare(
            diff_tool,
            head(trace["a"], SAVE_2, d / "a_head.txt"),
            head(trace["b"], SAVE_2, d / "b_head.txt"),
            f"A and B to tick {SAVE_2}",
        )
        same = compare(diff_tool, trace["b"], trace["c"], "B and C") and same
        return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
