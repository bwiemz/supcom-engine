#!/usr/bin/env python3
"""Compare two checksum traces (opensupcom --checksum-trace) tick by tick.

Each line is "tick total rng armies entities" (hex). The first tick whose
parts differ is where two runs -- two processes, two platforms, a replay and
its recording -- stopped playing the same game, and the part says where to
look: `rng` (one side rolled more or fewer numbers), `armies` (economy or
army state), `entities` (a unit's position, health or existence).

    tools/checksum_diff.py a.txt b.txt    # exit 0 if identical, 1 if not
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

PARTS = ("total", "rng", "armies", "entities")


@dataclass(frozen=True)
class Tick:
    tick: int
    parts: tuple[str, ...]


def read_trace(path: Path) -> list[Tick]:
    ticks: list[Tick] = []
    for n, line in enumerate(path.read_text().splitlines(), start=1):
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 1 + len(PARTS):
            sys.exit(f"{path}:{n}: expected 'tick {' '.join(PARTS)}', got {line!r}")
        ticks.append(Tick(int(fields[0]), tuple(fields[1:])))
    return ticks


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    a_path, b_path = Path(sys.argv[1]), Path(sys.argv[2])
    a, b = read_trace(a_path), read_trace(b_path)
    if not a or not b:
        print(f"empty trace: {a_path if not a else b_path}")
        return 1
    for ta, tb in zip(a, b):
        if ta.tick != tb.tick:
            print(f"traces are out of step: tick {ta.tick} vs {tb.tick}")
            return 1
        if ta.parts != tb.parts:
            differing = [
                name for name, x, y in zip(PARTS[1:], ta.parts[1:], tb.parts[1:]) if x != y
            ]
            print(f"first divergence at tick {ta.tick}: {', '.join(differing)} differ")
            for name, x, y in zip(PARTS, ta.parts, tb.parts):
                print(f"  {name:9} {x}  {y}{'' if x == y else '  <-'}")
            return 1
    if len(a) != len(b):
        print(
            f"identical for {min(len(a), len(b))} ticks, then one trace ends "
            + f"({len(a)} vs {len(b)} ticks)"
        )
        return 1
    print(f"identical: {len(a)} ticks, final checksum {a[-1].parts[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
