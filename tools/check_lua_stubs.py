#!/usr/bin/env python3
"""Fail on a Lua binding that does nothing and isn't listed as a stub.

A binding whose body only reads its arguments, logs and returns 0 passes the
coverage ratchet (data.binding_coverage checks that a name exists), and a
script calling it just loses what it asked for. tools/lua_stub_baseline.txt
lists the stubs there are, each as "<Lua name> <C++ function>"; this check
fails on a do-nothing binding missing from it, and on a listed one that now
does something or is gone.

tools/lua_stub_accepted.txt lists the do-nothing bindings that are right as
they are, each with its reason after a '#': Moho's own method does nothing,
or the binding is a fallback for a function retail's Lua defines itself.

A body does nothing when it makes no call but reading arguments, pushing
nothing and logging, assigns nothing, and returns 0 on every path; a
lua_stubs:: helper counts too.

Usage: check_lua_stubs.py <repo root>   (exit 1 on a problem)
       check_lua_stubs.py --list <repo root>
       check_lua_stubs.py --self-test
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

READS = re.compile(
    r"(lua_gettop|lua_type|lua_is\w+|lua_to\w+|luaL_check\w+|luaL_opt\w+|static_cast"
    r"|spdlog::\w+|LOG\w*|warn_once|log_once|debug|trace|info|warn)$"
)
FUNC_DEF = re.compile(
    r"(?:static\s+|inline\s+)*int\s+([A-Za-z_][\w:]*)\s*\(\s*lua_State\s*\*\s*"
    r"(?:/\*\s*\w*\s*\*/)?\s*\w*\s*\)\s*\{"
)
STUB_ALIAS = re.compile(
    r"(?:constexpr\s+auto|(?:static\s+)?int\s*\(\s*\*\s*const)\s+(\w+)\s*\)?"
    r"(?:\s*\(lua_State\s*\*\))?\s*=\s*lua_stubs::(\w+)"
)
REGISTER = re.compile(r'register_function\(\s*"(\w+)"\s*,\s*')
REGISTER_TABLE = re.compile(r'register_table_function\(\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*')
REG_ENTRY = re.compile(r'\{\s*"([A-Za-z_]\w*)"\s*,\s*')


def match_brace(text: str, i: int) -> int:
    depth = 0
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        elif c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
        elif text.startswith("//", i):
            nl = text.find("\n", i)
            i = len(text) if nl < 0 else nl
        i += 1
    return i


def does_nothing(body: str) -> bool:
    b = re.sub(r"//[^\n]*", "", body)
    b = re.sub(r"/\*.*?\*/", "", b, flags=re.S)
    calls = set(re.findall(r"([A-Za-z_][\w:]*)\s*\(", b)) - {"if", "return", "for", "while",
                                                             "switch", "sizeof"}
    calls |= set(re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)\s*\(", b))
    if any(not READS.match(c) for c in calls):
        return False
    if re.search(r"(?<![=!<>])=(?!=)|\+\+|--|\+=|-=", re.sub(r'"(?:[^"\\]|\\.)*"', "", b)):
        return False
    returns = re.findall(r"return\s+([^;]+);", b)
    return all(r.strip() == "0" for r in returns)


def registrations(sources: dict[str, str]) -> list[tuple[str, str, str, str]]:
    """(file, Lua name, C++ function, body or '@stub') for each registration."""
    defs: dict[str, list[tuple[str, str]]] = {}
    for f, t in sources.items():
        for m in FUNC_DEF.finditer(t):
            b = m.end() - 1
            defs.setdefault(m.group(1).split("::")[-1], []).append((f, t[b + 1:match_brace(t, b)]))
        for m in STUB_ALIAS.finditer(t):
            defs.setdefault(m.group(1), []).append((f, "@stub"))
    out = []
    for f, t in sources.items():
        found = [(m, m.group(1)) for m in REGISTER.finditer(t)]
        found += [(m, f"{m.group(1)}.{m.group(2)}") for m in REGISTER_TABLE.finditer(t)]
        found += [(m, m.group(1)) for m in REG_ENTRY.finditer(t)
                  if re.match(r"(\[|[A-Za-z_][\w:]*\s*\}|lua_stubs::)", t[m.end():m.end() + 80])]
        for m, name in found:
            rest = t[m.end():m.end() + 2000].lstrip()
            if rest.startswith("lua_stubs::"):
                out.append((f, name, re.match(r"lua_stubs::(\w+)", rest).group(1), "@stub"))
            elif rest.startswith("["):
                b = rest.find("{")
                if b >= 0:
                    out.append((f, name, "<lambda>", rest[b + 1:match_brace(rest, b)]))
            else:
                fm = re.match(r"([A-Za-z_][\w:]*)", rest)
                fn = fm.group(1).split("::")[-1] if fm else ""
                cands = defs.get(fn, [])
                body = next((b for df, b in cands if df == f), cands[0][1] if cands else None)
                if body is not None:
                    out.append((f, name, fn, body))
    return out


def stubs_of(sources: dict[str, str]) -> set[str]:
    return {f"{name} {fn}" for _, name, fn, body in registrations(sources)
            if body == "@stub" or does_nothing(body)}


def sources_of(root: Path) -> dict[str, str]:
    return {str(f.relative_to(root)): f.read_text(errors="ignore")
            for f in sorted((root / "src").rglob("*")) if f.suffix in (".cpp", ".hpp")}


def read_names(path: Path) -> set[str]:
    out = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(" ".join(line.split()))
    return out


def read_accepted(path: Path) -> dict[str, str]:
    """Each accepted entry, and its reason (the text after its '#')."""
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        if line.lstrip().startswith("#"):
            continue
        entry, _, reason = line.partition("#")
        entry = " ".join(entry.split())
        if entry:
            out[entry] = reason.strip()
    return out


def problems_of(stubs: set[str], baseline: set[str],
                accepted: dict[str, str] | None = None) -> list[str]:
    accepted = accepted or {}
    out = [f"{s}: a binding that does nothing; implement it, or list it in "
           "tools/lua_stub_baseline.txt" for s in sorted(stubs - baseline - accepted.keys())]
    out += [f"{s}: no longer a stub; take it off tools/lua_stub_baseline.txt"
            for s in sorted(baseline - stubs)]
    out += [f"{s}: no longer a stub; take it off tools/lua_stub_accepted.txt"
            for s in sorted(accepted.keys() - stubs)]
    out += [f"{s}: in both tools/lua_stub_baseline.txt and tools/lua_stub_accepted.txt"
            for s in sorted(baseline & accepted.keys())]
    out += [f"{s}: accepted without a reason; give it one after a '#' in "
            "tools/lua_stub_accepted.txt" for s, why in sorted(accepted.items()) if not why]
    return out


def self_test() -> int:
    nothing = ["return 0;", "(void)L; return 0;", "spdlog::debug(\"x\"); return 0;",
               "if (lua_gettop(L) < 1) { return 0; } return 0;"]
    something = ["lua_pushnil(L); return 1;", "unit->x = 1; return 0;", "do_it(L); return 0;",
                 "count++; return 0;"]
    for b in nothing:
        if not does_nothing(b):
            print(f"self-test: '{b}' should do nothing")
            return 1
    for b in something:
        if does_nothing(b):
            print(f"self-test: '{b}' does something")
            return 1
    src = {"a.cpp": 'static int l_A(lua_State*) { return 0; }\n'
                    'static int l_B(lua_State* L) { lua_pushnil(L); return 1; }\n'
                    'void reg(S& s) { s.register_function("A", l_A); s.register_function("B", l_B);\n'
                    '  s.register_function("C", [](lua_State*) { return 0; }); }\n'
                    'static const luaL_Reg m[] = {{"D", lua_stubs::noop}, {"E", l_B}};\n'}
    got = stubs_of(src)
    want = {"A l_A", "C <lambda>", "D noop"}
    if got != want:
        print(f"self-test: stubs_of gave {sorted(got)}, want {sorted(want)}")
        return 1
    if len(problems_of({"A l_A"}, {"A l_A", "Z l_Z"})) != 1:
        print("self-test: stale baseline entry")
        return 1
    cases = [
        ({"A l_A", "B l_B"}, {"A l_A"}, {"B l_B": "Moho's does nothing"}, 0),
        ({"A l_A"}, set(), {"A l_A": ""}, 1),
        ({"A l_A"}, {"A l_A"}, {"A l_A": "why"}, 1),
        ({"A l_A"}, {"A l_A"}, {"Z l_Z": "why"}, 1),
    ]
    for stubs, baseline, accepted, n in cases:
        if len(problems_of(stubs, baseline, accepted)) != n:
            print(f"self-test: stubs {sorted(stubs)}, baseline {sorted(baseline)}, accepted "
                  f"{accepted}: want {n} problem(s)")
            return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) == 2 and argv[0] == "--list":
        for s in sorted(stubs_of(sources_of(Path(argv[1])))):
            print(s)
        return 0
    if len(argv) != 1:
        print(__doc__)
        return 2
    root = Path(argv[0])
    problems = problems_of(stubs_of(sources_of(root)),
                           read_names(root / "tools" / "lua_stub_baseline.txt"),
                           read_accepted(root / "tools" / "lua_stub_accepted.txt"))
    for p in problems:
        print(p)
    if problems:
        return 1
    print("lua stubs: each do-nothing binding is in the baseline or accepted")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
