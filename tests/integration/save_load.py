"""A saved game loads and plays on as the game did (CTest: data.save_load,
and data.save_load_replay).

Three processes, each writing `--checksum-trace`:

  A  plays the game and saves it after tick SAVE_1 (`--save-at`).
  B  loads A's save (`--load`), plays on, and saves again after tick SAVE_2:
     a save made in a loaded game.
  C  loads B's save and plays on.
  D  loads A's save as another installation would (its own user folder):
     A's snapshot isn't its own, so it catches up instead (not --by-replay).
  E  loads A's save in A's installation, as another binary of the same build
     would (a copy of the game with another linker build id, as every dirty
     build of a commit is, or its Debug build for its Release): A's snapshot
     names C functions by their place in A's binary, so E catches up too.

A load restores the save's snapshot (M208c), whose checksum must be the one
the save's history holds for its tick; with `--by-replay` it catches up from
the history instead, checking every tick against it. Either exits non-zero
on a difference. With `--scripted-orders`, each save is made with one of the
player's orders still to run, which only the save carries. Then B's trace
must match A's from SAVE_1 to SAVE_2 (after it, B gave an order A never did),
and C's must match B's after SAVE_2: domain by domain, with
tools/checksum_diff.py.

Usage:
    save_load.py [--by-replay] <opensupcom> <checksum_diff.py> -- <game args...>

The game args must include `--ticks` past SAVE_2. Exits 77 (skipped) when
the game has no data to run on.
"""

from __future__ import annotations

import re
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


def rebuilt(exe: Path, out: Path) -> Path:
    """A copy of `exe` as another binary of its build would be, its code
    where it was: its ELF build-id note (in its first pages) flipped, else
    (no note: the engine knows the binary by its file's digest) a byte more."""
    data = bytearray(exe.read_bytes())
    note = re.search(rb"\x04\x00\x00\x00(.{4})\x03\x00\x00\x00GNU\x00", data[: 1 << 16], re.DOTALL)
    if note:
        size = int.from_bytes(note.group(1), "little")
        for i in range(note.end(), note.end() + size):
            data[i] ^= 0xFF
    else:
        data.append(0)
    _ = out.write_bytes(bytes(data))
    out.chmod(0o755)
    return out


def between(trace: Path, first_tick: int, last_tick: int, out: Path) -> Path:
    """`trace` from `first_tick` to `last_tick`, both included (a restored
    game's trace starts after its saved tick)."""
    lines: list[str] = []
    for line in trace.read_text().splitlines():
        if line.startswith("#") or first_tick <= int(line.split()[0]) <= last_tick:
            lines.append(line)
    _ = out.write_text("\n".join(lines) + "\n")
    return out


def compare(diff_tool: Path, a: Path, b: Path, what: str) -> bool:
    diff = run([sys.executable, str(diff_tool), str(a), str(b)])
    print(f"{what}: {diff.stdout.strip()}")
    return diff.returncode == 0


def loaded(proc: subprocess.CompletedProcess[str], name: str, tick: int, by_replay: bool) -> bool:
    lines = [line for line in proc.stdout.splitlines() if line.startswith("LOAD")]
    for line in lines:
        print(f"{name}: {line}")
    if proc.returncode != 0:
        print(f"{name} failed: exit {proc.returncode}")
        return False
    if f"LOAD resumed tick={tick}" not in lines:
        print(f"{name} never took over the game at tick {tick}")
        return False
    # By the way asked for: restored at once, or caught up
    how = f"catching up to tick {tick}" if by_replay else f"restored at tick {tick}"
    if how not in proc.stdout:
        print(f"{name} wasn't {how.split(' at ')[0].split(' to ')[0]}")
        return False
    return True


def main(argv: list[str]) -> int:
    by_replay = bool(argv) and argv[0] == "--by-replay"
    if by_replay:
        argv = argv[1:]
    if len(argv) < 3 or "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    exe, diff_tool = Path(argv[0]), Path(argv[1])
    game_args = argv[argv.index("--") + 1 :]
    load_mode = ["--load-by-replay"] if by_replay else []
    with tempfile.TemporaryDirectory(prefix="osc-save-load-") as tmp:
        d = Path(tmp)
        save_1, save_2 = d / "one.oscsave", d / "two.oscsave"
        trace = {name: d / f"{name}.txt" for name in "abc"}
        # One installation: its key signs the snapshots it restores
        user = ["--user-dir", str(d / "user")]

        a = run(
            [
                str(exe),
                *game_args,
                *user,
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
                *load_mode,
                *user,
                *load_args(game_args),
                "--save",
                str(save_2),
                "--save-at",
                str(SAVE_2),
                "--checksum-trace",
                str(trace["b"]),
            ]
        )
        if not loaded(b, "B", SAVE_1, by_replay) or not save_2.exists():
            return 1
        c = run(
            [
                str(exe),
                "--load",
                str(save_2),
                *load_mode,
                *user,
                *load_args(game_args),
                "--checksum-trace",
                str(trace["c"]),
            ]
        )
        if not loaded(c, "C", SAVE_2, by_replay):
            return 1

        end = 1 << 31
        same = compare(
            diff_tool,
            between(trace["a"], SAVE_1 + 1, SAVE_2, d / "a_part.txt"),
            between(trace["b"], SAVE_1 + 1, SAVE_2, d / "b_part.txt"),
            f"A and B from tick {SAVE_1 + 1} to {SAVE_2}",
        )
        same = (
            compare(
                diff_tool,
                between(trace["b"], SAVE_2 + 1, end, d / "b_tail.txt"),
                between(trace["c"], SAVE_2 + 1, end, d / "c_tail.txt"),
                f"B and C after tick {SAVE_2}",
            )
            and same
        )
        if not by_replay:
            # Another installation (its own user folder, its own key) doesn't
            # trust A's snapshot: it catches up from the history instead, to
            # the same game.
            d_trace = d / "d.txt"
            other = run(
                [
                    str(exe),
                    "--load",
                    str(save_1),
                    *load_args(game_args),
                    "--user-dir",
                    str(d / "other"),
                    "--checksum-trace",
                    str(d_trace),
                ]
            )
            if not loaded(other, "D", SAVE_1, by_replay=True):
                return 1
            same = (
                compare(
                    diff_tool,
                    between(trace["b"], SAVE_1 + 1, SAVE_2, d / "b_part2.txt"),
                    between(d_trace, SAVE_1 + 1, SAVE_2, d / "d_part.txt"),
                    "B and D (another installation's load, caught up)",
                )
                and same
            )
            # Another binary of the same build, in A's installation: it
            # catches up too, where restoring would call A's C functions'
            # places in its own code
            e_trace = d / "e.txt"
            other_binary = run(
                [
                    str(rebuilt(exe, d / exe.name)),
                    "--load",
                    str(save_1),
                    *load_args(game_args),
                    *user,
                    "--checksum-trace",
                    str(e_trace),
                ]
            )
            if not loaded(other_binary, "E", SAVE_1, by_replay=True):
                return 1
            if "taken by another binary" not in other_binary.stdout:
                print("E didn't say A's snapshot was another binary's")
                return 1
            same = (
                compare(
                    diff_tool,
                    between(trace["b"], SAVE_1 + 1, SAVE_2, d / "b_part3.txt"),
                    between(e_trace, SAVE_1 + 1, SAVE_2, d / "e_part.txt"),
                    "B and E (another binary's load, caught up)",
                )
                and same
            )
        return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
