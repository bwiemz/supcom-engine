#!/usr/bin/env python3
"""clang-tidy ratchet: findings may only go down.

Runs clang-tidy (the checks in .clang-tidy) over src/ and tests/ using a
build's compile_commands.json, counts findings per (file, check), and
compares them with tools/clang_tidy_baseline.txt. A count above its
baseline fails; a count below it passes and asks for the baseline to be
lowered (--update), so a fix can't be undone later.

Counts are per file and check, not per line, so editing a file elsewhere
never turns an old finding into a "new" one.

    tools/clang_tidy_ratchet.py -p build/linux-debug           # check
    tools/clang_tidy_ratchet.py -p build/linux-debug --update  # rewrite baseline
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import cast

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "clang_tidy_baseline.txt"
WARNING = re.compile(
    r"^(?P<path>[^:\s]+):(?P<line>\d+):(?P<col>\d+): warning: .* \[(?P<check>[\w.,-]+)\]$"
)
Key = tuple[str, str]


def find_tool(names: list[str]) -> str:
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    sys.exit(f"none of {', '.join(names)} found on PATH")


def tool_version(clang_tidy: str) -> str:
    out = subprocess.run(
        [clang_tidy, "--version"], capture_output=True, text=True, check=False
    ).stdout
    match = re.search(r"version (\d+)", out)
    return match.group(1) if match else "?"


def sources_in(build: Path) -> int:
    """How many of this checkout's src/ and tests/ files the build compiles."""
    entries = cast(list[dict[str, str]], json.loads((build / "compile_commands.json").read_text()))
    prefixes = (str(ROOT / "src") + os.sep, str(ROOT / "tests") + os.sep)
    return sum(
        1
        for e in entries
        if os.path.normpath(os.path.join(e.get("directory", ""), e.get("file", ""))).startswith(
            prefixes
        )
    )


def run_tidy(build: Path, jobs: int) -> collections.Counter[Key]:
    run = find_tool(["run-clang-tidy", "run-clang-tidy.py"])
    clang_tidy = find_tool(["clang-tidy"])
    files = f"{ROOT}/(src|tests)/"
    proc = subprocess.run(
        [run, "-p", str(build), "-j", str(jobs), "-quiet", "-clang-tidy-binary", clang_tidy, files],
        capture_output=True,
        text=True,
        cwd=ROOT,
        check=False,
    )
    # Findings alone exit 0 (WarningsAsErrors is empty). Anything else means
    # files weren't analyzed -- a compile error, a stale compile_commands.json
    # -- and an unfinished run must not pass as "everything was fixed", nor be
    # written as the baseline.
    if proc.returncode != 0:
        sys.exit(
            f"run-clang-tidy exited {proc.returncode}: the analysis did not complete\n"
            + proc.stdout[-4000:]
            + proc.stderr[-4000:]
        )
    # A header's finding is reported once per file that includes it, under
    # different relative spellings (src/lua/../renderer/x.hpp).
    seen: set[tuple[str, str, str, str]] = set()
    counts: collections.Counter[Key] = collections.Counter()
    for line in (proc.stdout + proc.stderr).splitlines():
        match = WARNING.match(line.strip())
        if not match:
            continue
        path = os.path.relpath(os.path.normpath(match["path"]), ROOT)
        where = (path, match["line"], match["col"], match["check"])
        if where in seen or not path.startswith(("src/", "tests/")):
            continue
        seen.add(where)
        counts[(path, match["check"])] += 1
    return counts


def read_baseline() -> tuple[str, collections.Counter[Key]]:
    version = "?"
    counts: collections.Counter[Key] = collections.Counter()
    if not BASELINE.exists():
        return version, counts
    for line in BASELINE.read_text().splitlines():
        if line.startswith("# clang-tidy"):
            version = line.split()[-1]
        if not line.strip() or line.startswith("#"):
            continue
        count, path, check = line.split()
        counts[(path, check)] = int(count)
    return version, counts


def write_baseline(version: str, counts: collections.Counter[Key]) -> None:
    lines = [
        "# clang-tidy findings accepted when the ratchet started; they may only go down.",
        "# Rewritten by tools/clang_tidy_ratchet.py --update. Format: count path check",
        f"# clang-tidy {version}",
    ]
    for (path, check), count in sorted(counts.items()):
        if count:
            lines.append(f"{count} {path} {check}")
    _ = BASELINE.write_text("\n".join(lines) + "\n")


@dataclass(frozen=True)
class Args:
    build: Path
    jobs: int
    update: bool


def parse_args() -> Args:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    _ = parser.add_argument(
        "-p", "--build", type=Path, required=True, help="build dir with compile_commands.json"
    )
    _ = parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    _ = parser.add_argument(
        "--update", action="store_true", help="rewrite the baseline with the current counts"
    )
    ns = parser.parse_args()
    return Args(build=cast(Path, ns.build), jobs=cast(int, ns.jobs), update=cast(bool, ns.update))


def main() -> int:
    args = parse_args()

    if not (args.build / "compile_commands.json").exists():
        sys.exit(f"{args.build}/compile_commands.json not found (configure the build first)")
    # A build of another checkout lists none of these files: clang-tidy would
    # analyze nothing and every finding would look fixed.
    if sources_in(args.build) == 0:
        sys.exit(f"{args.build} compiles no file of this checkout ({ROOT})")
    version = tool_version(find_tool(["clang-tidy"]))
    current = run_tidy(args.build, args.jobs)

    if args.update:
        write_baseline(version, current)
        print(f"baseline rewritten: {sum(current.values())} findings")
        return 0

    base_version, baseline = read_baseline()
    if base_version != version:
        print(
            f"note: baseline is from clang-tidy {base_version}, this is {version}; "
            + "checks differ between versions"
        )
    worse = {k: (baseline[k], n) for k, n in current.items() if n > baseline[k]}
    better = {k: (n, current[k]) for k, n in baseline.items() if current[k] < n}
    for (path, check), (was, now) in sorted(worse.items()):
        print(f"NEW  {path} [{check}]: {was} -> {now}")
    for (path, check), (was, now) in sorted(better.items()):
        print(f"FIXED {path} [{check}]: {was} -> {now}")
    print(f"clang-tidy: {sum(current.values())} findings (baseline {sum(baseline.values())})")
    if worse:
        print("New clang-tidy findings: fix them (see the full output with run-clang-tidy).")
        return 1
    if better:
        print("Fewer findings than the baseline: lower it with --update and commit it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
