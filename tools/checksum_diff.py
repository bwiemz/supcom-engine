#!/usr/bin/env python3
"""Compare two checksum traces (opensupcom --checksum-trace) tick by tick.

A trace starts with a header naming its columns ("# tick total rng armies
entities units orders ..."), then one line per tick, in hex. The first tick
whose parts differ is where two runs -- two processes, two platforms, a replay
and its recording -- stopped playing the same game, and the domains that
differ say where to look: `rng` (one side rolled more or fewer numbers),
`armies` (economy or army state), `entities` (a position, health or
existence), `orders`, `weapons`, `threads`, and so on (see
SimState::ChecksumParts). A trace without a header is the old four-column
one ("tick total rng armies entities").

    tools/checksum_diff.py a.txt b.txt    # exit 0 if identical, 1 if not
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

LEGACY_PARTS = ("total", "rng", "armies", "entities")


@dataclass(frozen=True)
class Tick:
    tick: int
    parts: tuple[str, ...]


@dataclass(frozen=True)
class Trace:
    names: tuple[str, ...]  # "total", then each domain
    ticks: list[Tick]


def read_trace(path: Path) -> Trace:
    names = LEGACY_PARTS
    ticks: list[Tick] = []
    for n, line in enumerate(path.read_text().splitlines(), start=1):
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "#":
            if fields[1:2] != ["tick"]:
                sys.exit(f"{path}:{n}: expected '# tick total ...', got {line!r}")
            names = tuple(fields[2:])
            continue
        if len(fields) != 1 + len(names):
            sys.exit(f"{path}:{n}: expected 'tick {' '.join(names)}', got {line!r}")
        ticks.append(Tick(int(fields[0]), tuple(fields[1:])))
    return Trace(names, ticks)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    a_path, b_path = Path(sys.argv[1]), Path(sys.argv[2])
    a, b = read_trace(a_path), read_trace(b_path)
    if not a.ticks or not b.ticks:
        print(f"empty trace: {a_path if not a.ticks else b_path}")
        return 1
    if a.names != b.names:
        print(f"traces name different parts: {' '.join(a.names)} vs {' '.join(b.names)}")
        return 1
    names = a.names
    width = max(len(name) for name in names)
    for ta, tb in zip(a.ticks, b.ticks):
        if ta.tick != tb.tick:
            print(f"traces are out of step: tick {ta.tick} vs {tb.tick}")
            return 1
        if ta.parts != tb.parts:
            differing = [
                name for name, x, y in zip(names[1:], ta.parts[1:], tb.parts[1:]) if x != y
            ]
            print(f"first divergence at tick {ta.tick}: {', '.join(differing)} differ")
            for name, x, y in zip(names, ta.parts, tb.parts):
                print(f"  {name:{width}} {x}  {y}{'' if x == y else '  <-'}")
            return 1
    if len(a.ticks) != len(b.ticks):
        print(
            f"identical for {min(len(a.ticks), len(b.ticks))} ticks, then one trace ends "
            + f"({len(a.ticks)} vs {len(b.ticks)} ticks)"
        )
        return 1
    print(f"identical: {len(a.ticks)} ticks, final checksum {a.ticks[-1].parts[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
