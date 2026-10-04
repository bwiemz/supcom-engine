#!/usr/bin/env python3
"""Fail on an engine callback retail FA runs that the engine never names.

Moho runs script functions by name: methods on the objects it made
(unit:OnStartReclaim, an Edit's OnTextChanged...) and functions of UI modules
(gamemain.lua:OnBeat...). The coverage ratchet (data.binding_coverage) checks
the other direction, engine functions the scripts call; a callback the engine
never makes passes it, and the script's behaviour is just missing.

tools/retail_hooks.txt lists the callbacks: the On* names the executable holds
that retail Lua defines as functions, and the module functions it reports
"Error running" for. A callback counts as made when its name is a string
word in a string literal in src/ (a call by name, or one in Lua the engine
runs). tools/retail_hooks_baseline.txt lists those not made yet; the
check fails on one missing from it, and on one it lists that src/ now names.

Usage: check_retail_hooks.py <repo root>        (exit 1 on a problem)
       check_retail_hooks.py --extract <FA dir> (writes the list to stdout)
       check_retail_hooks.py --self-test
"""

from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
EXE_ON = re.compile(rb"On[A-Z][A-Za-z0-9_]*")
EXE_MODULE = re.compile(rb"Error running '(/[^':]+\.lua):([A-Za-z_][A-Za-z0-9_]*)")
LUA_DEF = re.compile(
    r"function\s+[\w.]*[:.](On[A-Z]\w*)\s*\("
    r"|\b(On[A-Z]\w*)\s*=\s*function\s*\("
    r"|^\s*function\s+(On[A-Z]\w*)\s*\(",
    re.M,
)


def strings(data: bytes) -> list[bytes]:
    return re.findall(rb"[\x20-\x7e]{4,}", data)


def lua_defined(sources: list[str]) -> set[str]:
    names: set[str] = set()
    for s in sources:
        s = re.sub(r"--[^\n]*", "", s)
        for m in LUA_DEF.finditer(s):
            names.add(m.group(1) or m.group(2) or m.group(3))
    return names


def hooks_from(exe: bytes, sources: list[str]) -> list[str]:
    runs = strings(exe)
    on_names = {r.decode() for r in runs if EXE_ON.fullmatch(r)}
    methods = sorted(on_names & lua_defined(sources))
    modules = sorted({f"{m.group(1).decode()}:{m.group(2).decode()}"
                      for r in runs for m in [EXE_MODULE.search(r)] if m})
    return [f"method {n}" for n in methods] + [f"module {m}" for m in modules]


def extract(fa: Path) -> int:
    exe = (fa / "bin" / "SupremeCommander.exe").read_bytes()
    sources = []
    for scd in sorted((fa / "gamedata").glob("*.scd")):
        with zipfile.ZipFile(scd) as z:
            for name in z.namelist():
                if name.lower().endswith(".lua"):
                    sources.append(z.read(name).decode("latin-1"))
    print("# Engine callbacks retail FA runs by name: tools/check_retail_hooks.py --extract")
    for line in hooks_from(exe, sources):
        print(line)
    return 0


def read_names(path: Path) -> list[str]:
    out = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def named(hook: str) -> str:
    return hook.split(" ", 1)[1].rsplit(":", 1)[-1]


def words_in_literals(text: str) -> set[str]:
    found: set[str] = set()
    for lit in LITERAL.findall(text):
        found.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", lit))
    return found


def literals(root: Path) -> set[str]:
    found: set[str] = set()
    for f in sorted((root / "src").rglob("*")):
        if f.suffix in (".cpp", ".hpp", ".h", ".cc"):
            found.update(words_in_literals(f.read_text(errors="ignore")))
    return found


def problems_of(hooks: list[str], baseline: list[str], made: set[str]) -> list[str]:
    out = []
    known = set(hooks)
    for h in hooks:
        if named(h) not in made and h not in baseline:
            out.append(f"{h}: retail runs it and src/ never names it; make the call, "
                       "or list it in tools/retail_hooks_baseline.txt")
    for h in baseline:
        if h not in known:
            out.append(f"{h}: in the baseline but not in tools/retail_hooks.txt")
        elif named(h) in made:
            out.append(f"{h}: src/ names it now; take it off tools/retail_hooks_baseline.txt")
    return out


def check(root: Path) -> list[str]:
    tools = root / "tools"
    return problems_of(read_names(tools / "retail_hooks.txt"),
                       read_names(tools / "retail_hooks_baseline.txt"), literals(root))


def self_test() -> int:
    exe = b"\x00OnStartReclaim\x00OnNothing\x00Error running '/lua/ui/game/gamemain.lua:OnBeat'\x00"
    lua = ["function Unit:OnStartReclaim(self, target)\n-- function Unit:OnNothing()\nend"]
    got = hooks_from(exe, lua)
    want = ["method OnStartReclaim", "module /lua/ui/game/gamemain.lua:OnBeat"]
    if got != want:
        print(f"self-test: hooks_from gave {got}, want {want}")
        return 1
    hooks = ["method OnA", "method OnB", "module /m.lua:OnC"]
    cases = [
        ({"OnA", "OnB", "OnC"}, [], 0),
        ({"OnA", "OnC"}, [], 1),
        ({"OnA", "OnC"}, ["method OnB"], 0),
        ({"OnA", "OnB", "OnC"}, ["method OnB"], 1),
        ({"OnA", "OnB", "OnC"}, ["method OnZ"], 1),
    ]
    for made, baseline, n in cases:
        if len(problems_of(hooks, baseline, made)) != n:
            print(f"self-test: made {sorted(made)}, baseline {baseline}: want {n} problem(s)")
            return 1
    if words_in_literals('call("OnA"); run("import(\'/m.lua\').OnC()");') != {"OnA", "import", "m", "lua", "OnC"}:
        print("self-test: literals")
        return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) == 2 and argv[0] == "--extract":
        return extract(Path(argv[1]))
    if len(argv) != 1:
        print(__doc__)
        return 2
    problems = check(Path(argv[0]))
    for p in problems:
        print(p)
    if problems:
        return 1
    print(f"retail hooks: {len(read_names(Path(argv[0]) / 'tools' / 'retail_hooks.txt'))} "
          "callbacks, each made or in the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
