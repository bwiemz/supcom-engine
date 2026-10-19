#!/usr/bin/env python3
"""Fail on engine code that reads the Lua state's blueprints itself.

The blueprint tables are keyed by lowercase id; retail's scripts name
blueprints in any case ('XEA3204'). blueprints::push_blueprint (in
src/blueprints/blueprint_store.cpp) is the one place that looks a blueprint
up by id, matching it in any case, and push_blueprints gives the whole table.
A raw lookup elsewhere misses an upper-case id and the script reading it gets
nil (an Engineering Station's pod rebuild died on that), so this check fails
on any other source under src/ that names the `__blueprints` global.

Usage: check_blueprint_lookups.py <repo root>   (exit 1 on a raw lookup)
       check_blueprint_lookups.py --self-test
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

OWNER = "src/blueprints/blueprint_store.cpp"
SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".inl", ".mm"}

_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
_GLOBAL = re.compile(r'"__blueprints"')


def raw_lookups(text: str) -> list[int]:
    """The lines (1-based) of `text` that name the global outside comments."""
    code = _COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return [code.count("\n", 0, m.start()) + 1 for m in _GLOBAL.finditer(code)]


def check(root: Path) -> list[str]:
    problems = []
    for path in sorted((root / "src").rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.suffix not in SUFFIXES or rel == OWNER:
            continue
        for line in raw_lookups(path.read_text(encoding="utf-8", errors="replace")):
            problems.append(
                f"{rel}:{line}: raw __blueprints lookup; use blueprints::push_blueprint"
            )
    return problems


def self_test() -> int:
    text = (
        "// __blueprints keys are lowercase: \"__blueprints\"\n"
        "/* lua_pushstring(L, \"__blueprints\"); */\n"
        "lua_pushstring(L, \"__blueprints\");\n"
    )
    if raw_lookups(text) != [3]:
        print(f"self-test: raw_lookups gave {raw_lookups(text)}, want [3]")
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
