#!/usr/bin/env python3
"""Compare two checksum traces (opensupcom --checksum-trace) tick by tick.

A trace starts with a header naming its columns ("# tick total rng armies
entities units orders ..."), then one line per tick, in hex. The first tick
whose parts differ is where two runs -- two processes, two platforms, a replay
and its recording -- stopped playing the same game, and the domains that
differ say where to look: `rng` (one side rolled more or fewer numbers),
`armies` (economy or army state), `entities` (a position, health or
existence), `orders`, `weapons`, and so on (see SimState::ChecksumParts).
Diagnostic columns (`threads`, live script threads) aren't in the total:
they can differ where the games agree, as each client's focus army forks its
own script threads. The first divergence is reported among the compared
domains, and separately the first tick a diagnostic column differs, which is
where to look when a compared one follows. A trace without a header is the
old four-column one ("tick total rng armies entities").

    tools/checksum_diff.py a.txt b.txt    # exit 0 if identical, 1 if not
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

LEGACY_PARTS = ("total", "rng", "armies", "entities")
DIAGNOSTIC = frozenset({"threads"})


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


def first_difference(a: Trace, b: Trace, diagnostic: bool) -> Tick | None:
    """The first tick of `a` whose compared (or, with `diagnostic`, whose
    diagnostic) columns differ from `b`'s; traces must name the same parts."""
    columns = [i for i, name in enumerate(a.names) if (name in DIAGNOSTIC) == diagnostic]
    for ta, tb in zip(a.ticks, b.ticks):
        if any(ta.parts[i] != tb.parts[i] for i in columns):
            return ta
    return None


def show(a: Trace, b: Trace, tick: Tick, label: str) -> None:
    tb = next(t for t in b.ticks if t.tick == tick.tick)
    pairs = list(zip(a.names, tick.parts, tb.parts))
    differing = [name for name, x, y in pairs[1:] if x != y]
    print(f"{label} at tick {tick.tick}: {', '.join(differing)} differ")
    width = max(len(name) for name in a.names)
    for name, x, y in pairs:
        print(f"  {name:{width}} {x}  {y}{'' if x == y else '  <-'}")


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
    for ta, tb in zip(a.ticks, b.ticks):
        if ta.tick != tb.tick:
            print(f"traces are out of step: tick {ta.tick} vs {tb.tick}")
            return 1
    compared = first_difference(a, b, diagnostic=False)
    diagnostic = first_difference(a, b, diagnostic=True)
    if compared:
        show(a, b, compared, "first divergence")
    if diagnostic:
        show(a, b, diagnostic, "first diagnostic difference")
    if compared or diagnostic:
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
