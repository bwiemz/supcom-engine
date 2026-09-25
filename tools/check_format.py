#!/usr/bin/env python3
"""Formatting ratchet: lines changed since <base> must follow .clang-format.

Untouched code is never reformatted wholesale, and moved code counts as
changed only where it changed:
- the diff finds renames, and pairs a file rewritten in place (its content
  moved elsewhere) with where its content went;
- lines added as a block the diff also deletes somewhere (a function moved
  to another file, as a file is split) are moved, not changed. A block
  starts where MOVE_RUN lines in a row match MOVE_RUN deleted lines in a
  row, runs as far as they keep matching, and needs MOVE_ALNUM letters and
  digits in all (as git's moved-code detection asks), so braces alone never
  make one. Blank lines count as changed unless they moved: the style caps
  their number.

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
MOVE_RUN = 3
MOVE_ALNUM = 20


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


class Deleted:
    """The diff's deleted lines, indexed by their MOVE_RUN-line windows."""

    def __init__(self) -> None:
        self.lines: list[str | None] = []  # hunks in order, None between them
        self.windows: dict[tuple[str, ...], list[int]] = {}

    def add_hunk(self, hunk: list[str]) -> None:
        base = len(self.lines)
        self.lines.extend(hunk)
        self.lines.append(None)
        for i in range(len(hunk) - MOVE_RUN + 1):
            self.windows.setdefault(tuple(hunk[i : i + MOVE_RUN]), []).append(base + i)

    def moved(self, added: list[tuple[int, str]]) -> set[int]:
        """Line numbers of `added` (one hunk's lines, in order) that belong to
        a block moved from the deleted lines."""
        lines = [text for _, text in added]
        found: set[int] = set()
        i = 0
        while i <= len(lines) - MOVE_RUN:
            best = 0
            for p in self.windows.get(tuple(lines[i : i + MOVE_RUN]), [])[:64]:
                k = MOVE_RUN
                while i + k < len(lines) and self.lines[p + k] == lines[i + k]:
                    k += 1
                best = max(best, k)
            if best and sum(c.isalnum() for s in lines[i : i + best] for c in s) >= MOVE_ALNUM:
                found.update(n for n, _ in added[i : i + best])
                i += best
            else:
                i += 1
        return found


def changed_lines(base: str) -> dict[str, list[tuple[int, int]]]:
    """Each changed C/C++ file's added or changed line ranges (1-based, inclusive)."""
    # -B -M: a file rewritten in place gives up its content to where it
    # went, so a move is not a wholesale change. Deleted files, and files
    # whose pairing -B broke (B), are read too: what they held may have
    # moved.
    diff = git("diff", "-B", "-M", "-U0", "--no-color", "--diff-filter=ABCDMR", base, "--", *PATHS)
    hunks: list[tuple[str, list[tuple[int, str]]]] = []  # (path, added lines)
    deleted_index = Deleted()
    deleted: list[str] = []
    current: str | None = None
    number = 0

    def end_deleted() -> None:
        if deleted:
            deleted_index.add_hunk(deleted[:])
        deleted.clear()

    in_header = False  # between "diff --git" and the file's first hunk
    for line in diff.splitlines():
        if line.startswith("diff --git"):
            end_deleted()
            in_header = True
        elif in_header and line.startswith("+++ "):
            path = line[4:]
            current = path[2:] if path.startswith("b/") else None
            if current and Path(current).suffix not in EXTENSIONS:
                current = None
        elif in_header and not line.startswith("@@"):
            continue  # index, mode, rename and "---" lines
        elif m := HUNK.match(line):
            end_deleted()
            in_header = False
            number = int(m.group(1))
            if current:
                hunks.append((current, []))
        elif line.startswith("-"):
            deleted.append(line[1:])
        elif line.startswith("+"):
            end_deleted()
            if current and hunks:
                hunks[-1][1].append((number, line[1:]))
            number += 1
    end_deleted()

    ranges: dict[str, list[tuple[int, int]]] = {}
    for path, added in hunks:
        skip = deleted_index.moved(added)
        for n, _ in added:
            if n in skip:
                continue
            spans = ranges.setdefault(path, [])
            if spans and spans[-1][1] == n - 1:
                spans[-1] = (spans[-1][0], n)
            else:
                spans.append((n, n))
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
