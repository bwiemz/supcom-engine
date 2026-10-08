#!/usr/bin/env python3
"""The benchmarks (M223): time a pinned AI game, or frames of it, and hold
them to a baseline.

`opensupcom --bench <report.json>` times each headless tick (tick_stats in
src/app/bench.cpp). `opensupcom --render-bench <report.json>` renders a scene
of a saved game offscreen at 1920x1080 and times its frames on the CPU and the
GPU, with what they drew (src/app/render_bench.cpp, M223b). This plays the
pinned scenarios with them and compares the reports:

  Scenarios: early, late (the sim), early-moho, late-moho (the same with
  --moho-pathing); render-battle, render-late,
  render-strategic (the renderer: the pinned game saved at a tick, made once
  per machine beside the baselines, then the scene's camera path over it).

  run <opensupcom> <report.json> [--scenario S] [--repeat N]
      Play scenario S (default: early) N times (default 1); keep the fastest
      report. The runs must play the same game (one checksum).
  compare <baseline.json> <report.json> [--tolerance T]
      Hold the report to the baseline: sim time (total), the mean and p99
      tick, and peak memory may each be at most T (default 0.15) slower or
      larger; for a render scene, the CPU and GPU frame's p50 and p95 and
      the VRAM it allocated (its p99s are reported, not held: too noisy). Builds of another type, or another game (its checksum at the
      end), can't be compared: the game changed, so time it against the
      previous build's binary, or record a new baseline. A render scene must
      also be on the same GPU.
  check <opensupcom> [--scenario S] [--repeat N] [--update]
      run, then compare with <golden dir>/bench/<scenario>-<build type>.json
      (the goldens' folder: $OSC_GOLDEN_DIR, else <State>/opensupcom/golden),
      recording it when there is none (or with --update). CTest's bench.*.
  --self-test

Exit codes: 0 ok, 1 slower, 2 usage or not comparable, 77 no game data
(skipped).

Benchmarks want a quiet machine: CTest runs them one at a time (RUN_SERIAL),
and a Release build (Debug is several times slower, and its own baseline).
"""

from __future__ import annotations

import json
import os
import re
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
# The same games with Moho's pathing by footprint class (--moho-pathing, off
# by default until it is accepted): their reports also count the searches.
SCENARIOS["early-moho"] = [*SCENARIOS["early"], "--moho-pathing"]
SCENARIOS["late-moho"] = [*SCENARIOS["late"], "--moho-pathing"]
TIMEOUT_SECONDS = 3600

# The render scenes (M223b): the pinned game saved at a tick (made once, with
# the headless run's --save), and the scene rendered from it.
RENDER_SCENES: dict[str, tuple[str, int]] = {
    "render-battle": ("battle", 6000),
    "render-late": ("late", 18000),
    "render-strategic": ("strategic", 18000),
}

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

# A render scene's p99s are reported, not held: across runs of one scene
# they move 20-50% (a frame or two in 600), where p50 and p95 hold to about
# 10%.
RENDER_METRICS = (
    Metric("cpu p50", ("cpu", "p50_ms"), "ms"),
    Metric("cpu p95", ("cpu", "p95_ms"), "ms"),
    Metric("gpu p50", ("gpu", "p50_ms"), "ms"),
    Metric("gpu p95", ("gpu", "p95_ms"), "ms"),
    Metric("vram", ("vram_mb", "allocated_peak"), "MB"),
)


def is_render(report: Report) -> bool:
    return "render" in report


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


def game_of(report: Report) -> tuple[object, ...]:
    """What was timed: the game (its tick and checksum at the end); for a
    render scene also the scene, the tick it began at, and the GPU."""
    game = cast("dict[str, object]", report["game"])
    if is_render(report):
        render = cast("dict[str, object]", report["render"])
        return (
            render["scene"],
            game["start_tick"],
            game["end_tick"],
            game["checksum"],
            report.get("device"),
        )
    return game["tick"], game["checksum"]


def has(report: Report, path: tuple[str, ...]) -> bool:
    """A figure the report holds (a device without timestamps has no GPU times)."""
    try:
        _ = number(report, path)
    except (KeyError, TypeError):
        return False
    return True


def compare(baseline: Report, current: Report, tolerance: float) -> tuple[int, list[str]]:
    """(exit code, lines to print)."""
    if baseline.get("build_type") != current.get("build_type"):
        return 2, [
            f"not comparable: a {current.get('build_type')} build against a "
            + f"{baseline.get('build_type')} baseline"
        ]
    if game_of(baseline) != game_of(current):
        return 2, [
            f"not comparable: another game or scene ({game_of(current)}; the baseline's "
            + f"{game_of(baseline)})",
            "the sim or the scene changed: time it against the previous build's binary, "
            + "or record a new baseline (check --update)",
        ]
    lines = [f"{'':12} {'baseline':>12} {'now':>12} {'change':>8}"]
    slower = False
    for m in RENDER_METRICS if is_render(current) else METRICS:
        if not (has(baseline, m.path) and has(current, m.path)):
            lines.append(f"{m.name:12} {'(none)':>12} {'(none)':>12}")
            continue
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


# Each tick's saves of the builds timed last, the newest kept (each is
# ~10 MB; comparing two builds runs both).
KEEP_SAVES = 3


def version_build(text: str) -> str | None:
    """The build in `opensupcom --version`'s line ("OpenSupCom 0.1.0
    (cc67ae41-dirty)"), as a file name may carry it; None if it has none."""
    match = re.search(r"\(([^()]+)\)\s*$", text.strip())
    return re.sub(r"[^A-Za-z0-9._-]", "_", match.group(1)) if match else None


def save_name(tick: int, build: str | None) -> str:
    """The pinned game's save at `tick` by `build`: a save loads only in the
    build that made it (saved_game.cpp's WrongVersion)."""
    return f"scmp009-4ai-4242-t{tick}" + (f"-{build}" if build else "") + ".SCFAsave"


def render_save(exe: Path, tick: int) -> tuple[int, list[str]]:
    """(exit code, the engine's arguments to load it): the pinned game saved at
    `tick` by this build, beside the baselines, made the first time this build
    is timed. Saves and loads share one user folder, whose key signs the
    snapshot (another's would catch up by replay instead of restoring)."""
    folder = golden_dir() / "bench" / "saves"
    user = folder / "user"
    version = subprocess.run(
        [str(exe), "--version"], capture_output=True, text=True, timeout=60, check=False
    )
    save = folder / save_name(tick, version_build(version.stdout))
    if not save.exists():
        # Other builds' saves of this tick, past the newest few, go first
        others = [s for s in folder.glob(f"scmp009-4ai-4242-t{tick}*.SCFAsave") if s != save]
        others.sort(key=lambda s: s.stat().st_mtime, reverse=True)
        for old in others[KEEP_SAVES - 1 :]:
            old.unlink()
        user.mkdir(parents=True, exist_ok=True)
        print(f"saving the pinned game at tick {tick} (once): {save}")
        proc = subprocess.run(
            [
                str(exe),
                "--user-dir",
                str(user),
                *SCENARIOS["early"][:-4],  # the map and its AIs
                "--ticks",
                str(tick),
                "--seed",
                "4242",
                "--save",
                str(save),
                "--save-at",
                str(tick),
            ],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        if proc.returncode == SKIPPED:
            return SKIPPED, []
        if proc.returncode != 0 or not save.exists():
            print(f"saving failed: exit {proc.returncode}")
            print(proc.stdout[-2000:])
            return 1, []
    return 0, ["--user-dir", str(user), "--load", str(save)]


def fastest(report: Report) -> float:
    """What `run` keeps the least of: sim time, or a render scene's CPU p50."""
    return number(report, ("cpu", "p50_ms") if is_render(report) else ("ticks", "total_ms"))


def run(exe: Path, scenario: str, repeat: int) -> tuple[int, Report | None]:
    """(exit code, the fastest run's report)."""
    best: Report | None = None
    if scenario in RENDER_SCENES:
        scene, tick = RENDER_SCENES[scenario]
        code, load = render_save(exe, tick)
        if code != 0:
            return code, None
        args = [*load, "--render-scene", scene]
        flag = "--render-bench"
    else:
        args = SCENARIOS[scenario]
        flag = "--bench"
    with tempfile.TemporaryDirectory(prefix="osc-bench-") as tmp:
        for i in range(repeat):
            report_file = Path(tmp) / f"report{i}.json"
            proc = subprocess.run(
                [str(exe), *args, flag, str(report_file)],
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
            if is_render(report):
                print(
                    f"run {i + 1}/{repeat}: cpu p50 {fastest(report):.2f} ms, "
                    + f"{game_of(report)}"
                )
            else:
                total = number(report, ("ticks", "total_ms"))
                print(f"run {i + 1}/{repeat}: {total / 1000:.1f} s of sim, game {game_of(report)}")
            if best is not None and game_of(best) != game_of(report):
                print("the runs played different games: the sim isn't deterministic here")
                return 1, None
            if best is None or fastest(report) < fastest(best):
                best = report
    return 0, best


def golden_dir() -> Path:
    """Where baselines live: the goldens' folder, as --golden finds it
    ($OSC_GOLDEN_DIR, else <State>/opensupcom/golden)."""
    if golden := os.environ.get("OSC_GOLDEN_DIR"):
        return Path(golden)
    if sys.platform == "win32":
        state = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    else:
        state = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state")
    return state / "opensupcom" / "golden"


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

    def render(cpu: float, gpu: float | None, vram: float = 500, device: str = "gpu") -> Report:
        timed: Report = {"p50_ms": gpu, "p95_ms": gpu, "p99_ms": gpu} if gpu else {}
        return {
            "build_type": "release",
            "device": device,
            "render": {"scene": "battle"},
            "cpu": {"p50_ms": cpu, "p95_ms": cpu, "p99_ms": cpu},
            "gpu": timed or None,
            "vram_mb": {"allocated_peak": vram},
            "game": {"start_tick": 6000, "end_tick": 6100, "checksum": "ab"},
        }

    rbase = render(5.0, 1.0)
    cases += [
        (compare(rbase, render(5.5, 1.1), 0.15)[0], 0),  # within 15%
        (compare(rbase, render(6.5, 1.0), 0.15)[0], 1),  # CPU 30% slower
        (compare(rbase, render(5.0, 1.5), 0.15)[0], 1),  # GPU 50% slower
        (compare(rbase, render(5.0, None), 0.15)[0], 0),  # no GPU times: not held
        (compare(rbase, render(5.0, 1.0, device="other"), 0.15)[0], 2),  # another GPU
    ]
    # A save is named for the build that made it
    cases += [
        (version_build("OpenSupCom 0.1.0 (cc67ae41-dirty)") == "cc67ae41-dirty", True),
        (version_build("OpenSupCom 0.1.0 (a b/c)\n") == "a_b_c", True),
        (version_build("OpenSupCom 0.1.0") is None, True),
        (save_name(6000, "cc67ae41") == "scmp009-4ai-4242-t6000-cc67ae41.SCFAsave", True),
        (save_name(6000, None) == "scmp009-4ai-4242-t6000.SCFAsave", True),
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
    if (scenario not in SCENARIOS and scenario not in RENDER_SCENES) or repeat < 1:
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
        golden = golden_dir()
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
