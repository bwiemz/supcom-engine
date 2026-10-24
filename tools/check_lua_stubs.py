#!/usr/bin/env python3
"""Fail on a Lua binding that does nothing and isn't listed as a stub.

A binding whose body only reads its arguments, logs and returns 0 passes the
coverage ratchet (data.binding_coverage checks that a name exists), and a
script calling it just loses what it asked for. So does one that answers a
constant, nil included, where Moho's answers what is so. tools/
lua_stub_baseline.txt lists the stubs there are, each as "<Lua name> <C++
function>"; this check fails on a stand-in missing from it, and on a listed
one that now does something or is gone.

tools/lua_stub_accepted.txt lists the stand-ins that are right as they are,
each with its reason after a '#': Moho's own does nothing or answers the
same constant, or the binding is a fallback for a function retail's Lua
defines itself.

The source is read through tools/cpp_source.py, so a comment or a literal
holds no registration and no brace.

A body does nothing when it makes no call but reading arguments, pushing
nothing and logging, assigns nothing, and returns 0 on every path; a
lua_stubs:: helper counts too. It answers a constant when it does that but
for pushing literals (nil, a literal boolean, number or string, an empty
table) and returning their count. A binding is found registered by
register_function, register_table_function, a {"name", fn} entry, a raw
lua_pushstring("name") + lua_pushcfunction, or as an entry of the user
state's stand-in table (src/lua/user_stand_ins.cpp: "<name> <kind>").

Usage: check_lua_stubs.py <repo root>   (exit 1 on a problem)
       check_lua_stubs.py --list <repo root>
       check_lua_stubs.py --self-test
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# The checks run as scripts, so tools/ is sys.path[0]: the module beside them
import cpp_source  # pyright: ignore[reportImplicitRelativeImport]

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
# lua_pushstring(L, "Name"); lua_pushcfunction(L, fn) -- then a raw set
RAW_REGISTER = re.compile(
    r'lua_pushstring\(\s*\w+\s*,\s*"([A-Za-z_]\w*)"\s*\)\s*;\s*'
    + r"lua_push(?:cfunction|cclosure)\(\s*\w+\s*,\s*"
)
# An entry of the user state's stand-in table: every one is a stand-in
STAND_IN = re.compile(
    r'\{\s*"([A-Za-z_]\w*)"\s*,\s*(Command|ReturnsBool|GlobalString|GlobalStrings)\s*\{'
)
# A push of a literal: what a constant answer pushes
CONST_PUSH = re.compile(
    r"lua_(?:pushnil|newtable)\(\s*\w+\s*\)"
    + r"|lua_pushboolean\(\s*\w+\s*,\s*(?:0|1|true|false)\s*\)"
    + r"|lua_pushnumber\(\s*\w+\s*,\s*-?[\d.]+[fF]?\s*\)"
    + r'|lua_pushstring\(\s*\w+\s*,\s*"(?:[^"\\]|\\.)*"\s*\)'
)


def match_brace(code: str, i: int) -> int:
    """The brace closing the one at `i` in `code` (cpp_source's: no comment or
    literal left to hold a brace)."""
    depth = 0
    while i < len(code):
        c = code[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return i


def does_nothing(body: str) -> bool:
    b = cpp_source.scan(body).code
    calls = set(re.findall(r"([A-Za-z_][\w:]*)\s*\(", b)) - {"if", "return", "for", "while",
                                                             "switch", "sizeof"}
    calls |= set(re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)\s*\(", b))
    if any(not READS.match(c) for c in calls):
        return False
    if re.search(r"(?<![=!<>])=(?!=)|\+\+|--|\+=|-=", b):  # (a literal's contents are blank)
        return False
    returns = re.findall(r"return\s+([^;]+);", b)
    return all(r.strip() == "0" for r in returns)


def answers_constant(body: str) -> bool:
    """Whether the body's only answers are literals it pushes."""
    b = cpp_source.scan(body).code
    pushed = CONST_PUSH.sub("", b)
    if pushed == b:
        return False
    returns: list[str] = re.findall(r"return\s+([^;]+);", pushed)
    if not returns or not all(re.fullmatch(r"\d+", r.strip()) for r in returns):
        return False
    return does_nothing(re.sub(r"return\s+\d+\s*;", "return 0;", pushed))


def registrations(sources: dict[str, str]) -> list[tuple[str, str, str, str]]:
    """(file, Lua name, C++ function, body or '@stub') for each registration."""
    # The source as cpp_source reads it: definitions are found in the code,
    # registrations (which name their binding in a literal) in the text at a
    # place that is code, and bodies are read from the code.
    scanned = {f: cpp_source.scan(t) for f, t in sources.items()}
    defs: dict[str, list[tuple[str, str]]] = {}
    for f, src in scanned.items():
        code = src.code
        for m in FUNC_DEF.finditer(code):
            b = m.end() - 1
            defs.setdefault(m.group(1).split("::")[-1], []).append((f, code[b + 1:match_brace(code, b)]))
        for m in STUB_ALIAS.finditer(code):
            defs.setdefault(m.group(1), []).append((f, "@stub"))
    out: list[tuple[str, str, str, str]] = []
    for f, src in scanned.items():
        t, code = src.text, src.code
        found = [(m, m.group(1)) for m in REGISTER.finditer(t)]
        found += [(m, f"{m.group(1)}.{m.group(2)}") for m in REGISTER_TABLE.finditer(t)]
        found += [(m, m.group(1)) for m in REG_ENTRY.finditer(t)
                  if re.match(r"(\[|[A-Za-z_][\w:]*\s*\}|lua_stubs::)", code[m.end():m.end() + 80])]
        found += [(m, m.group(1)) for m in RAW_REGISTER.finditer(t)]
        found = [(m, name) for m, name in found if src.in_code(m.start())]
        out += [(f, m.group(1), m.group(2), "@stub") for m in STAND_IN.finditer(t)
                if src.in_code(m.start())]
        for m, name in found:
            rest = code[m.end():m.end() + 2000].lstrip().lstrip("+")
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
            if body == "@stub" or does_nothing(body) or answers_constant(body)}


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
    constant = ["lua_pushnil(L); return 1;", "lua_pushboolean(L, 0); return 1;",
                "lua_newtable(L); return 1;", "(void)L; lua_pushnumber(L, 0.5); return 1;",
                'lua_pushstring(L, "us"); return 1;',
                "if (lua_gettop(L) > 0) { lua_pushnil(L); return 1; } lua_pushnil(L); return 1;"]
    varying = ["return 0;", "lua_pushboolean(L, on ? 1 : 0); return 1;",
               "lua_pushnumber(L, sim->tick()); return 1;", "lua_pushvalue(L, 1); return 1;",
               "lua_newtable(L); lua_pushnumber(L, 0); lua_rawseti(L, -2, 1); return 1;",
               "lua_pushnil(L); return n;"]
    for b in nothing:
        if not does_nothing(b):
            print(f"self-test: '{b}' should do nothing")
            return 1
    for b in something:
        if does_nothing(b):
            print(f"self-test: '{b}' does something")
            return 1
    for b in constant:
        if not answers_constant(b):
            print(f"self-test: '{b}' answers a constant")
            return 1
    for b in varying:
        if answers_constant(b):
            print(f"self-test: '{b}' answers no constant")
            return 1
    src = {"a.cpp": 'static int l_A(lua_State*) { return 0; }\n'
                    'static int l_B(lua_State* L) { lua_pushnil(L); return 1; }\n'
                    'static int l_F(lua_State* L) { lua_pushnumber(L, x); return 1; }\n'
                    'void reg(S& s) { s.register_function("A", l_A); s.register_function("B", l_B);\n'
                    '  s.register_function("C", [](lua_State*) { return 0; }); }\n'
                    'static const luaL_Reg m[] = {{"D", lua_stubs::noop}, {"E", l_F}};\n'
                    'void raw(lua_State* L) {\n'
                    '  lua_pushstring(L, "R"); lua_pushcfunction(L, l_A);\n'
                    '  lua_rawset(L, LUA_GLOBALSINDEX);\n'
                    '  lua_pushstring(L, "N");\n'
                    '  lua_pushcfunction(L, +[](lua_State* c) { lua_pushnil(c); return 1; });\n'
                    '  lua_rawset(L, LUA_GLOBALSINDEX);\n'
                    '  lua_pushstring(L, "G"); lua_pushcfunction(L, l_F);\n'
                    '  lua_rawset(L, LUA_GLOBALSINDEX); }\n'
                    'constexpr UserStandIn k[] = {{"P", Command{}}, {"Q", ReturnsBool{false}},\n'
                    '                             {"__w", GlobalString{"us"}}};\n'
                    # a brace in a block comment or a literal is no brace, and a
                    # registration in a comment is none
                    'static int l_X(lua_State* L) { /* } */ do_it(L); return 0; }\n'
                    'static int l_Y(lua_State* L) { lua_pushstring(L, "}"); do_it(L); return 1; }\n'
                    'void more(S& s) { s.register_function("X", l_X);\n'
                    '  s.register_function("Y", l_Y);\n'
                    '  // s.register_function("Gone", l_A);\n'
                    '  /* {"Old", lua_stubs::noop}, */ }\n'}
    got = stubs_of(src)
    want = {"A l_A", "B l_B", "C <lambda>", "D noop", "R l_A", "N <lambda>", "P Command",
            "Q ReturnsBool", "__w GlobalString"}
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
