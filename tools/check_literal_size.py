#!/usr/bin/env python3
"""Fail on a string literal MSVC won't compile.

MSVC caps one string literal at 16,380 bytes (error C2026) and a run of
adjacent literals, concatenated, at 65,535 (C1091). GCC and Clang take
either, so a shader source grown on Linux breaks only the Windows build
(it has: mesh_frag, #143). This check runs with the architecture tests on
every platform, so it fails where the growth is made.

A raw string's line breaks are counted two bytes each: a Windows checkout
may give the file CRLF line ends, which a raw string can keep, so the count
is the one the Windows build could see.

Usage: check_literal_size.py <dir or file>...   (exit 1 on any over)
       check_literal_size.py --self-test
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

PIECE_LIMIT = 16_380  # C2026: one literal
RUN_LIMIT = 65_535  # C1091: adjacent literals, concatenated
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}

# An encoding prefix, then R for a raw string: R"delim( ... )delim".
_RAW_OPEN = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')


@dataclass(frozen=True)
class Literal:
    line: int
    size: int  # its characters' bytes, as written (a raw string's line breaks as CRLF)
    start: int
    end: int


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    size: int
    limit: int
    what: str

    def describe(self) -> str:
        return (
            f"{self.path}:{self.line}: {self.what} of {self.size} bytes (MSVC's limit {self.limit})"
        )


def literals(text: str) -> list[Literal]:
    """The string literals in C++ source `text`, skipping comments and
    character literals."""
    found: list[Literal] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == "R" or (c in "uUL" and _RAW_OPEN.match(text, i)):
            raw = _RAW_OPEN.match(text, i)
            if raw and not _ident_before(text, i):
                close = ")" + raw.group(1) + '"'
                j = text.find(close, raw.end())
                if j < 0:
                    break
                body = text[raw.end() : j]
                size = len(body.encode()) + body.count("\n")  # its line breaks as CRLF
                found.append(Literal(text.count("\n", 0, i) + 1, size, i, j + len(close)))
                i = j + len(close)
            else:
                i += 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            body = text[i + 1 : j]
            found.append(Literal(text.count("\n", 0, i) + 1, len(body.encode()), i, j + 1))
            i = j + 1
        elif c == "'" and not _in_number(text, i):
            # A character literal (a digit separator, 1'000, is not one).
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1
    return found


def _ident_before(text: str, i: int) -> bool:
    """Whether the R at `i` (after any prefix) ends an identifier, as in
    `FOR"`… — then it is no raw string."""
    k = i
    while k > 0 and text[k - 1] in "u8UL":
        k -= 1
    return k > 0 and (text[k - 1].isalnum() or text[k - 1] == "_")


def _in_number(text: str, i: int) -> bool:
    """Whether the quote at `i` is a digit separator (`1'000`, `0xFF'FF`):
    the word before it starts with a digit. `L'x'` and `u8'x'` are
    character literals."""
    k = i
    while k > 0 and (text[k - 1].isalnum() or text[k - 1] in "_'"):
        k -= 1
    return k < i and text[k].isdigit()


def _only_space_or_comments(between: str) -> bool:
    stripped = re.sub(r"//[^\n]*|/\*.*?\*/", "", between, flags=re.DOTALL)
    return stripped.strip() == ""


def check_text(path: Path, text: str) -> list[Finding]:
    findings: list[Finding] = []
    lits = literals(text)
    for lit in lits:
        if lit.size > PIECE_LIMIT:
            findings.append(
                Finding(path, lit.line, lit.size, PIECE_LIMIT, "string literal (C2026)")
            )
    # Runs of adjacent literals, which the compiler concatenates.
    run_start = 0
    for k in range(1, len(lits) + 1):
        joined = k < len(lits) and _only_space_or_comments(text[lits[k - 1].end : lits[k].start])
        if not joined:
            total = sum(lit.size for lit in lits[run_start:k])
            if k - run_start > 1 and total > RUN_LIMIT:
                findings.append(
                    Finding(
                        path, lits[run_start].line, total, RUN_LIMIT, "concatenated literal (C1091)"
                    )
                )
            run_start = k
    return findings


def check_paths(roots: list[Path]) -> list[Finding]:
    findings: list[Finding] = []
    for root in roots:
        files = (
            [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.suffix in SUFFIXES)
        )
        for f in files:
            findings += check_text(f, f.read_text(encoding="utf-8", errors="replace"))
    return findings


def self_test() -> int:
    big = "x" * (PIECE_LIMIT + 1)
    half = "y" * (PIECE_LIMIT - 10)
    cases: list[tuple[str, str, int]] = [
        ("an oversized raw string", f'const char* s = R"glsl({big})glsl";', 1),
        ("an oversized plain string", f'const char* s = "{big}";', 1),
        (
            "a raw string split in two",
            f'const char* s = R"glsl({half})glsl" // split\n R"glsl({half})glsl";',
            0,
        ),
        ("a raw string at the limit", f'auto s = R"({"z" * PIECE_LIMIT})";', 0),
        (
            "a raw string over the limit only with CRLF line breaks",
            'auto s = R"(' + ("w" * 39 + "\n") * 400 + ')";',
            1,
        ),
        ("in a comment", f'// R"glsl({big})glsl"\n/* "{big}" */ int x;', 0),
        ("after a quote in a char literal", f'char q = \'"\'; auto s = R"({big})";', 1),
        ("after a digit separator", f'int k = 1\'000; auto s = "{big}";', 1),
        ("an identifier ending in R", f'#define FOR(x) x\nint FOR"a"; auto s = "{big}";', 1),
        (
            "a concatenation over 64 KB",
            "auto s = " + " ".join(f'R"({half})"' for _ in range(5)) + ";",
            1,
        ),
        ("a u8 raw string", f'auto s = u8R"({big})";', 1),
        ("after a wide char literal's quote", f'wchar_t q = L\'"\'; auto s = "{big}";', 1),
        ("a delimiter's parenthesis in the body", f'auto s = R"d(a)b)d"; auto t = "{big}";', 1),
    ]
    failed = 0
    for name, text, want in cases:
        got = len(check_text(Path("<self-test>"), text))
        if got != want:
            print(f"self-test FAILED: {name}: {got} finding(s), expected {want}")
            failed += 1
    lits = literals('int a; auto s = "a\\"b"; auto t = R"x(q)x";')
    if [lit.size for lit in lits] != [4, 1]:
        print(f"self-test FAILED: escaped quote and raw sizes: {[lit.size for lit in lits]}")
        failed += 1
    print(f"self-test: {len(cases) + 1 - failed}/{len(cases) + 1} passed")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if not argv:
        print(__doc__)
        return 2
    findings = check_paths([Path(a) for a in argv])
    for f in findings:
        print(f.describe())
    if findings:
        advice = 'split a raw string into adjacent literals (`)glsl" R"glsl(`) at a blank line.'
        print(f"{len(findings)} literal(s) over MSVC's limits: {advice}")
        return 1
    print("string literals: all within MSVC's limits")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
