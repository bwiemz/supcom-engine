#!/usr/bin/env python3
"""The sim benchmark (M223): time a pinned AI game and hold it to a baseline.

`opensupcom --bench <report.json>` times each headless tick (tick_stats in
src/app/bench.cpp). This plays the pinned scenarios with it and compares the
reports:

  run <opensupcom> <report.json> [--scenario S] [--repeat N]
      Play scenario S (default: early) N times (default 1); keep the fastest
      report. The runs must play the same game (one checksum).
  compare <baseline.json> <report.json> [--tolerance T]
      Hold the report to the baseline: sim time (total), the mean and p99
      tick, and peak memory may each be at most T (default 0.15) slower or
      larger. Builds of another type, or another game (its checksum at the
      end), can't be compared: the game changed, so time it against the
      previous build's binary, or record a new baseline.
  check <opensupcom> [--scenario S] [--repeat N] [--update]
      run, then compare with $OSC_GOLDEN_DIR/bench/<scenario>-<build type>.json,
      recording it when there is none (or with --update). CTest's bench.*.
  --self-test

Exit codes: 0 ok, 1 slower, 2 usage or not comparable, 77 no game data or
no $OSC_GOLDEN_DIR (skipped).

Benchmarks want a quiet machine: CTest runs them one at a time (RUN_SERIAL),
and a Release build (Debug is several times slower, and its own baseline).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import cast

SKIPPED = 77
MAP = "/maps/SCMP_009/SCMP_009_scenario.lua"
SCENARIOS: dict[str, list[str]] = {
    # Four AIs on Seton's Clutch: the early game (10 minutes) ...
    "early": [
        "--map",
        MAP,
        "--ai-skirmish",
        "--ai-armies",
        "4",
        "--ticks",
        "6000",
        "--seed",
        "4242",
    ],
    # ... and into the late game, where the AI's queries and Lua's GC grow
    "late": [
        "--map",
        MAP,
        "--ai-skirmish",
        "--ai-armies",
        "4",
        "--ticks",
        "18000",
        "--seed",
        "4242",
    ],
}
TIMEOUT_SECONDS = 3600

Report = dict[str, object]


@dataclass(frozen=True)
class Metric:
    name: str
    path: tuple[str, ...]  # where it is in the report
    unit: str


METRICS = (
    Metric("sim time", ("ticks", "total_ms"), "ms"),
    Metric("mean tick", ("ticks", "mean_ms"), "ms"),
    Metric("p99 tick", ("ticks", "p99_ms"), "ms"),
    Metric("peak memory", ("peak_memory_mb",), "MB"),
)


def lookup(report: Report, path: tuple[str, ...]) -> object:
    table: Report = report
    for key in path[:-1]:
        inner = table[key]
        if not isinstance(inner, dict):
            raise TypeError(f"{key} is not a table")
        table = cast("Report", inner)
    return table[path[-1]]


def number(report: Report, path: tuple[str, ...]) -> float:
    value = lookup(report, path)
    if not isinstance(value, (int, float)):
        raise TypeError(f"{'.'.join(path)} is not a number")
    return float(value)


def game_of(report: Report) -> tuple[object, object]:
    game = cast("dict[str, object]", report["game"])
    return game["tick"], game["checksum"]


def compare(baseline: Report, current: Report, tolerance: float) -> tuple[int, list[str]]:
    """(exit code, lines to print)."""
    if baseline.get("build_type") != current.get("build_type"):
        return 2, [
            f"not comparable: a {current.get('build_type')} build against a "
            + f"{baseline.get('build_type')} baseline"
        ]
    if game_of(baseline) != game_of(current):
        (bt, bc), (ct, cc) = game_of(baseline), game_of(current)
        return 2, [
            f"not comparable: another game (tick {ct}, checksum {cc}; the baseline's "
            + f"tick {bt}, checksum {bc})",
            "the sim changed: time it against the previous build's binary, "
            + "or record a new baseline (check --update)",
        ]
    lines = [f"{'':12} {'baseline':>12} {'now':>12} {'change':>8}"]
    slower = False
    for m in METRICS:
        was, now = number(baseline, m.path), number(current, m.path)
        change = (now - was) / was if was > 0 else 0.0
        mark = ""
        if change > tolerance:
            mark = f"  over +{tolerance:.0%}"
            slower = True
        lines.append(
            f"{m.name:12} {was:>9.1f} {m.unit:2} {now:>9.1f} {m.unit:2} {change:>+7.1%}{mark}"
        )
    return (1 if slower else 0), lines


def run(exe: Path, scenario: str, repeat: int) -> tuple[int, Report | None]:
    """(exit code, the fastest run's report)."""
    best: Report | None = None
    with tempfile.TemporaryDirectory(prefix="osc-bench-") as tmp:
        for i in range(repeat):
            report_file = Path(tmp) / f"report{i}.json"
            proc = subprocess.run(
                [str(exe), *SCENARIOS[scenario], "--bench", str(report_file)],
                cwd=tmp,
                capture_output=True,
                text=True,
                timeout=TIMEOUT_SECONDS,
                check=False,
            )
            if proc.returncode == SKIPPED:
                print("no game data: skipped")
                return SKIPPED, None
            if proc.returncode != 0 or not report_file.exists():
                print(f"run {i + 1} failed: exit {proc.returncode}")
                print(proc.stdout[-2000:])
                return 1, None
            report = cast("Report", json.loads(report_file.read_text(encoding="utf-8")))
            total = number(report, ("ticks", "total_ms"))
            print(f"run {i + 1}/{repeat}: {total / 1000:.1f} s of sim, game {game_of(report)}")
            if best is not None and game_of(best) != game_of(report):
                print("the runs played different games: the sim isn't deterministic here")
                return 1, None
            if best is None or total < number(best, ("ticks", "total_ms")):
                best = report
    return 0, best


def option(args: list[str], name: str, default: str) -> str:
    if name in args:
        at = args.index(name)
        if at + 1 < len(args):
            return args[at + 1]
    return default


def self_test() -> int:
    def report(
        total: float, mean: float, p99: float, memory: float, checksum: str = "ab"
    ) -> Report:
        return {
            "build_type": "release",
            "ticks": {"total_ms": total, "mean_ms": mean, "p99_ms": p99},
            "peak_memory_mb": memory,
            "game": {"tick": 6000, "checksum": checksum},
        }

    base = report(1000, 1.0, 10, 500)
    cases = [
        (compare(base, report(1100, 1.1, 11, 520), 0.15)[0], 0),  # within 15%
        (compare(base, report(1200, 1.0, 10, 500), 0.15)[0], 1),  # sim time 20% slower
        (compare(base, report(1000, 1.0, 12, 500), 0.15)[0], 1),  # p99 20% slower
        (compare(base, report(1000, 1.0, 10, 600), 0.15)[0], 1),  # 20% more memory
        (compare(base, report(700, 0.7, 7, 400), 0.15)[0], 0),  # faster
        (compare(base, report(1000, 1.0, 10, 500, "cd"), 0.15)[0], 2),  # another game
        (compare(base, {**report(1000, 1.0, 10, 500), "build_type": "debug"}, 0.15)[0], 2),
    ]
    failed = [i for i, (got, want) in enumerate(cases) if got != want]
    for i in failed:
        print(f"self-test case {i}: exit {cases[i][0]}, expected {cases[i][1]}")
    print("self-test:", "FAIL" if failed else "ok")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) < 3:
        print(__doc__)
        return 2
    command, args = argv[1], argv[2:]
    scenario = option(args, "--scenario", "early")
    repeat = int(option(args, "--repeat", "1"))
    if scenario not in SCENARIOS or repeat < 1:
        print(__doc__)
        return 2

    if command == "compare" and len(args) >= 2:
        baseline = cast("Report", json.loads(Path(args[0]).read_text(encoding="utf-8")))
        current = cast("Report", json.loads(Path(args[1]).read_text(encoding="utf-8")))
        code, lines = compare(baseline, current, float(option(args, "--tolerance", "0.15")))
        print("\n".join(lines))
        return code

    # (resolved: the game runs in a temporary working directory)
    exe = Path(args[0]).resolve()
    if not exe.is_file():
        print(f"no game binary at {exe}")
        return 2
    if command == "run" and len(args) >= 2:
        code, best = run(exe, scenario, repeat)
        if best is not None:
            _ = Path(args[1]).write_text(json.dumps(best, indent=2) + "\n", encoding="utf-8")
        return code

    if command == "check":
        golden = os.environ.get("OSC_GOLDEN_DIR")
        if not golden:
            print("no $OSC_GOLDEN_DIR for the baseline: skipped")
            return SKIPPED
        code, best = run(exe, scenario, repeat)
        if best is None:
            return code
        baseline_file = Path(golden) / "bench" / f"{scenario}-{best['build_type']}.json"
        if "--update" in args or not baseline_file.exists():
            baseline_file.parent.mkdir(parents=True, exist_ok=True)
            _ = baseline_file.write_text(json.dumps(best, indent=2) + "\n", encoding="utf-8")
            print(f"baseline recorded: {baseline_file}")
            return 0
        baseline = cast("Report", json.loads(baseline_file.read_text(encoding="utf-8")))
        code, lines = compare(baseline, best, float(option(args, "--tolerance", "0.15")))
        print("\n".join(lines))
        if code != 0:
            # This run's report beside the baseline, to look at or adopt
            fresh = baseline_file.with_name(baseline_file.stem + ".new.json")
            _ = fresh.write_text(json.dumps(best, indent=2) + "\n", encoding="utf-8")
            print(f"this run's report: {fresh}")
        return code

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
