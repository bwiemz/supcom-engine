#!/usr/bin/env python3
"""Formatting ratchet: lines changed since <base> must follow .clang-format.

Untouched code is never reformatted wholesale. A moved file's lines count as
changed only where they changed: the diff finds renames, and a file
rewritten in place (its content moved elsewhere) is paired with where its
content went.

    tools/check_format.py [base]         # base defaults to origin/main
    tools/check_format.py --fix [base]

CLANG_FORMAT picks the binary (CI pins the version the style was set with).
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import subprocess
import sys
from pathlib import Path

PATHS = ("src", "tests")
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def changed_lines(base: str) -> dict[str, list[tuple[int, int]]]:
    """Each changed C/C++ file's added or changed line ranges (1-based, inclusive)."""
    # -B -M: a file rewritten in place gives up its content to where it
    # went, so a move is not a wholesale change.
    diff = git("diff", "-B", "-M", "-U0", "--no-color", "--diff-filter=ACMR", base, "--", *PATHS)
    ranges: dict[str, list[tuple[int, int]]] = {}
    current: str | None = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            path = line[4:]
            current = path[2:] if path.startswith("b/") else None
            if current and Path(current).suffix not in EXTENSIONS:
                current = None
        elif current and (m := HUNK.match(line)):
            start = int(m.group(1))
            count = int(m.group(2)) if m.group(2) is not None else 1
            if count > 0:
                ranges.setdefault(current, []).append((start, start + count - 1))
    return ranges


def main() -> int:
    parser = argparse.ArgumentParser(description="Lines changed since base follow .clang-format.")
    _ = parser.add_argument("--fix", action="store_true", help="format the changed lines")
    _ = parser.add_argument("base", nargs="?", default="origin/main")
    args = parser.parse_args()
    fix = bool(args.fix)  # pyright: ignore[reportAny] -- argparse values are untyped
    base = str(args.base)  # pyright: ignore[reportAny] -- as above
    clang_format = os.environ.get("CLANG_FORMAT", "clang-format")

    os.chdir(git("rev-parse", "--show-toplevel").strip())
    if subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}"],
        capture_output=True,
        check=False,
    ).returncode:
        print(f"format: '{base}' is not a commit (fetch it, or pass another base)", file=sys.stderr)
        return 2

    differs = False
    for path, spans in sorted(changed_lines(base).items()):
        # Bytes: line endings and encodings are compared as they are.
        source = Path(path).read_bytes()
        result = subprocess.run(
            [clang_format, "--style=file", *(f"--lines={a}:{b}" for a, b in spans), path],
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            print(result.stderr.decode(errors="replace"), file=sys.stderr)
            print(f"format: clang-format failed on {path}", file=sys.stderr)
            return 2
        if result.stdout == source:
            continue
        differs = True
        if fix:
            _ = Path(path).write_bytes(result.stdout)
            print(f"formatted {path}")
            continue
        for chunk in difflib.unified_diff(
            source.decode(errors="replace").splitlines(keepends=True),
            result.stdout.decode(errors="replace").splitlines(keepends=True),
            f"a/{path}",
            f"b/{path}",
        ):
            _ = sys.stdout.write(chunk)
    if fix or not differs:
        if not differs:
            print("format: changed lines follow .clang-format")
        return 0
    print(
        f"format: changed lines differ from .clang-format; run tools/check_format.sh --fix {base}"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
