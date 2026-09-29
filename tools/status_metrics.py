#!/usr/bin/env python3
"""Keep the status numbers in the docs true.

docs/current-state.md states counts that every other change moves: unit
tests, the tests under each CTest label, the clang-tidy baseline, the retail
API still unbound. Written by hand they went stale within days. Each is now
written between markers, `<!-- metric:NAME -->value<!-- /metric -->`, and
this tool computes the values from the sources and a configured build:

    status_metrics.py update --build-dir build/linux-debug   # rewrite them
    status_metrics.py check  --build-dir build/linux-debug   # exit 1 if stale

`arch.status_metrics` runs `check` (on Linux and macOS: the counts are
those builds'), so a change that adds tests updates the doc in the same PR.

    status_metrics.py --self-test
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

DOCS = ["docs/current-state.md"]
_MARKER = re.compile(r"<!-- metric:([a-z_]+) -->(.*?)<!-- /metric -->", re.DOTALL)


def ctest_count(build_dir: Path, label: str) -> int:
    """The tests CTest has under `label` (registered: a data test counts
    whether or not this machine has the data)."""
    out = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-N", "-L", f"^{label}$"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    m = re.search(r"Total Tests: (\d+)", out)
    if not m:
        raise RuntimeError(f"ctest -N -L {label}: no total in its output")
    return int(m.group(1))


def unit_test_cases(build_dir: Path) -> int:
    exe = next(
        (
            p
            for p in (build_dir / "tests" / "osc_tests", build_dir / "tests" / "osc_tests.exe")
            if p.exists()
        ),
        None,
    )
    if exe is None:
        raise RuntimeError(f"no osc_tests under {build_dir / 'tests'} (build it first)")
    out = subprocess.run(
        [str(exe), "--list-tests"], capture_output=True, text=True, check=True
    ).stdout
    m = re.search(r"(\d+) (?:matching )?test cases?", out)
    if not m:
        raise RuntimeError("osc_tests --list-tests: no count in its output")
    return int(m.group(1))


def tidy_baseline(root: Path) -> int:
    total = 0
    for line in (root / "tools" / "clang_tidy_baseline.txt").read_text().splitlines():
        fields = line.split()
        if fields and not fields[0].startswith("#"):
            total += int(fields[0])
    return total


def unbound(root: Path, kind: str) -> int:
    lines = (
        (root / "tests" / "integration" / "binding_baseline_retail.txt").read_text().splitlines()
    )
    return sum(1 for line in lines if line.startswith(f"{kind} "))


def metrics(root: Path, build_dir: Path) -> dict[str, Callable[[], int]]:
    """Each metric, computed when asked (a doc may name only some)."""
    return {
        "unit_test_cases": lambda: unit_test_cases(build_dir),
        "gate_tests": lambda: ctest_count(build_dir, "gate"),
        "mp_tests": lambda: ctest_count(build_dir, "mp"),
        "arch_tests": lambda: ctest_count(build_dir, "arch"),
        "golden_tests": lambda: ctest_count(build_dir, "golden"),
        "tidy_baseline": lambda: tidy_baseline(root),
        "unbound_globals": lambda: unbound(root, "G"),
        "unbound_methods": lambda: unbound(root, "M"),
    }


def rewrite(text: str, values: Callable[[str], str]) -> str:
    """`text` with each marker's value from `values(name)`."""
    return _MARKER.sub(
        lambda m: f"<!-- metric:{m.group(1)} -->{values(m.group(1))}<!-- /metric -->", text
    )


def run(root: Path, build_dir: Path, write: bool) -> int:
    known = metrics(root, build_dir)
    cache: dict[str, str] = {}

    def value(name: str) -> str:
        if name not in known:
            raise RuntimeError(
                f"unknown metric '{name}' (tools/status_metrics.py knows {sorted(known)})"
            )
        if name not in cache:
            cache[name] = f"{known[name]():,}"
        return cache[name]

    stale = 0
    for doc in DOCS:
        path = root / doc
        text = path.read_text()
        new = rewrite(text, value)
        if new == text:
            continue
        if write:
            _ = path.write_text(new)
            print(f"{doc}: updated")
            continue
        for m in _MARKER.finditer(text):
            if m.group(2) != value(m.group(1)):
                stale += 1
                print(f"{doc}: {m.group(1)} says {m.group(2)}, is {value(m.group(1))}")
    if stale:
        print(f"run: tools/status_metrics.py update --build-dir {build_dir}")
        return 1
    if not write:
        print(f"status metrics: {len(cache)} current")
    return 0


def self_test() -> int:
    text = "a <!-- metric:x -->1<!-- /metric --> b <!-- metric:y -->old<!-- /metric -->"
    got = rewrite(text, lambda name: {"x": "1", "y": "2,345"}[name])
    want = "a <!-- metric:x -->1<!-- /metric --> b <!-- metric:y -->2,345<!-- /metric -->"
    if got != want:
        print(f"self-test: {got!r}")
        return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 3 or argv[0] not in ("update", "check") or argv[1] != "--build-dir":
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parent.parent
    return run(root, Path(argv[2]), write=argv[0] == "update")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
