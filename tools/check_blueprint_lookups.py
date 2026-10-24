#!/usr/bin/env python3
"""Fail on engine code that reads the Lua state's blueprints itself.

The blueprint tables are keyed by lowercase id; retail's scripts name
blueprints in any case ('XEA3204'). blueprints::push_blueprint (in
src/blueprints/blueprint_store.cpp) is the one place that looks a blueprint
up by id, matching it in any case. A raw lookup elsewhere misses an
upper-case id and the script reading it gets nil (an Engineering Station's
pod rebuild died on that), so this check fails on:

- `__blueprints` in the text of any string literal outside the owner: a C++
  lookup, a literal split in two ("__blue" "prints"), or a Lua chunk the
  engine runs;
- a call of push_blueprints (the whole table, keyed lowercase) outside the
  owner and the readers that walk or freeze the whole table.

Comments are skipped, and string literals are read as C++ reads them (a `//`
inside one is not a comment): tools/cpp_source.py reads the source.

Usage: check_blueprint_lookups.py <repo root>   (exit 1 on a raw lookup)
       check_blueprint_lookups.py --self-test
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

# The checks run as scripts, so tools/ is sys.path[0]: the module beside them
import cpp_source  # pyright: ignore[reportImplicitRelativeImport]

OWNER = "src/blueprints/blueprint_store.cpp"
# Files that may name the global, and why.
NAME_ALLOWED = {
    OWNER: "makes the table and owns the lookup",
    "src/app/mods_flow_test.cpp": "a test probe's Lua reads the table as a script does",
}
# Readers of the whole table.
BULK_ALLOWED = {
    OWNER,
    "src/blueprints/blueprint_store.hpp",
    "src/sim/build_site_props.cpp",  # walks every blueprint's categories
    "src/sim/sim_state.cpp",  # freezes the table (M224g)
}
SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".inl", ".mm"}

_BULK_CALL = re.compile(r"\bpush_blueprints\s*\(")


def scan(text: str) -> tuple[list[tuple[int, str]], str]:
    """The string literals of C++ `text` as (line, contents), adjacent ones
    joined; and the code with comments and literals blanked (lines kept)."""
    src = cpp_source.scan(text)
    literals: list[tuple[int, str]] = []
    after_last: int | None = None  # where the last literal ended
    for lit in src.literals:
        if after_last is not None and not src.code[after_last : lit.start].strip():
            literals[-1] = (literals[-1][0], literals[-1][1] + lit.body)
        else:
            literals.append((lit.line, lit.body))
        after_last = lit.end
    return literals, src.code


def problems_in(rel: str, text: str) -> list[str]:
    literals, code = scan(text)
    out: list[str] = []
    if rel not in NAME_ALLOWED:
        out += [
            f"{rel}:{ln}: raw __blueprints lookup; use blueprints::push_blueprint"
            for ln, body in literals
            if "__blueprints" in body
        ]
    if rel not in BULK_ALLOWED:
        out += [
            f"{rel}:{code.count(chr(10), 0, m.start()) + 1}: push_blueprints outside the "
            + "readers of the whole table; look one up with blueprints::push_blueprint"
            for m in _BULK_CALL.finditer(code)
        ]
    return out


def check(root: Path) -> list[str]:
    problems: list[str] = []
    scanned: set[str] = set()
    for path in sorted((root / "src").rglob("*")):
        if path.suffix in SUFFIXES:
            rel = path.relative_to(root).as_posix()
            scanned.add(rel)
            problems += problems_in(rel, path.read_text(encoding="utf-8", errors="replace"))
    if OWNER not in scanned:
        problems.append(
            f"{OWNER} was not scanned ({len(scanned)} files read under {root / 'src'}); "
            + "is this the repository root?"
        )
    return problems


def self_test() -> int:
    cases: list[tuple[str, str, list[int]]] = [
        ("a lookup", 'lua_pushstring(L, "__blueprints");\n', [1]),
        ("a comment", '// "__blueprints"\n/* lua_pushstring(L, "__blueprints"); */\n', []),
        ("a // inside a string", 'log("https://x"); push(L, "__blueprints");\n', [1]),
        ("a /* */ after a // string", 'log("//");\n/* "__blueprints" */\n', []),
        ("a split literal", 'f();\nlua_pushstring(L, "__blue"\n    "prints");\n', [2]),
        ("a Lua chunk", "f();\nrun(R\"(\n  local t = rawget(_G, '__blueprints')\n)\");\n", [2]),
        ("an escaped quote", 'f("a\\"b"); g("__blueprints");\n', [1]),
        ("a char literal", 'f(\'"\'); g("__blueprints");\n', [1]),
        ("a digit separator", 'n = 1\'000; g("__blueprints");\n', [1]),
        ("a literal after a keyword", 'return"//"; g("__blueprints");\n', [1]),
        ("other literals", 'f("__blueprint"); g("blueprints");\n', []),
    ]
    failed = 0
    for what, text, want in cases:
        got = [int(p.split(":")[1]) for p in problems_in("src/x.cpp", text)]
        if got != want:
            print(f"self-test: {what}: lines {got}, want {want}")
            failed += 1
    bulk = "void f(lua_State* L) {\n    if (blueprints::push_blueprints(L)) g();\n}\n"
    if [int(p.split(":")[1]) for p in problems_in("src/sim/unit.cpp", bulk)] != [2]:
        print("self-test: push_blueprints outside the bulk readers is refused")
        failed += 1
    if problems_in("src/sim/sim_state.cpp", bulk):
        print("self-test: push_blueprints in a bulk reader is allowed")
        failed += 1
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        if not check(root) or "was not scanned" not in check(root)[0]:
            print("self-test: a tree without the owner fails")
            failed += 1
        (root / OWNER).parent.mkdir(parents=True)
        _ = (root / OWNER).write_text('push(L, "__blueprints");\n')
        (root / "src/sim/unit.cpp").parent.mkdir(parents=True)
        _ = (root / "src/sim/unit.cpp").write_text('f();\nlua_pushstring(L, "__blueprints");\n')
        if [p.split(":")[:2] for p in check(root)] != [["src/sim/unit.cpp", "2"]]:
            print(f"self-test: the scan of a tree finds its raw lookup alone: {check(root)}")
            failed += 1
    if failed:
        return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    problems = check(Path(argv[0]))
    for p in problems:
        print(p)
    if problems:
        return 1
    print("blueprint_lookups: every blueprint lookup goes through blueprints::push_blueprint")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
