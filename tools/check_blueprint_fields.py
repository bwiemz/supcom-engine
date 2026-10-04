#!/usr/bin/env python3
"""Fail on a blueprint field only the engine reads that src/ never names.

Retail's blueprints set fields the engine acts on and no script reads: a
unit's flight model, its guard scan radius, a weapon slaved to the body. One
the engine ignores loads without a word, and the unit just behaves otherwise.

tools/blueprint_fields.txt lists the fields: set in retail's .bp files, named
in the executable, and read by no retail Lua. A field counts as read when its
name is a word in a string literal in src/. tools/blueprint_fields_baseline.txt
lists those not read yet; the check fails on one missing from it, and on one
it lists that src/ now names.

Usage: check_blueprint_fields.py <repo root>        (exit 1 on a problem)
       check_blueprint_fields.py --extract <FA dir> (writes the list to stdout)
       check_blueprint_fields.py --self-test
"""

from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def bp_keys(text: str) -> set[str]:
    text = re.sub(r"#[^\n]*", "", text)
    text = re.sub(r"'[^'\n]*'|\"[^\"\n]*\"", "''", text)
    return set(re.findall(r"(?m)(?:^|[{,])\s*([A-Za-z_]\w*)\s*=(?!=)", text))


def lua_reads(text: str) -> set[str]:
    text = re.sub(r"--[^\n]*", "", text)
    reads = set(re.findall(r"\.([A-Za-z_]\w*)\b", text))
    reads.update(re.findall(r"\[\s*['\"]([A-Za-z_]\w*)['\"]\s*\]", text))
    return reads


def fields_from(exe: bytes, bps: list[str], luas: list[str]) -> list[str]:
    in_exe = {m.decode() for m in re.findall(rb"[A-Za-z_][A-Za-z0-9_]{2,}", exe)}
    keys: set[str] = set()
    for t in bps:
        keys |= bp_keys(t)
    read: set[str] = set()
    for t in luas:
        read |= lua_reads(t)
    return sorted((keys & in_exe) - read)


def extract(fa: Path) -> int:
    exe = (fa / "bin" / "SupremeCommander.exe").read_bytes()
    bps: list[str] = []
    luas: list[str] = []
    for scd in sorted((fa / "gamedata").glob("*.scd")):
        with zipfile.ZipFile(scd) as z:
            for name in z.namelist():
                low = name.lower()
                if low.endswith(".bp"):
                    bps.append(z.read(name).decode("latin-1"))
                elif low.endswith(".lua"):
                    luas.append(z.read(name).decode("latin-1"))
    print("# Blueprint fields only the engine reads: tools/check_blueprint_fields.py --extract")
    for f in fields_from(exe, bps, luas):
        print(f)
    return 0


def read_names(path: Path) -> list[str]:
    out = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def words_in_literals(text: str) -> set[str]:
    found: set[str] = set()
    for lit in LITERAL.findall(text):
        found.update(WORD.findall(lit))
    return found


def literals(root: Path) -> set[str]:
    found: set[str] = set()
    for f in sorted((root / "src").rglob("*")):
        if f.suffix in (".cpp", ".hpp", ".h", ".cc"):
            found.update(words_in_literals(f.read_text(errors="ignore")))
    return found


def problems_of(fields: list[str], baseline: list[str], read: set[str]) -> list[str]:
    out = []
    known = set(fields)
    for f in fields:
        if f not in read and f not in baseline:
            out.append(f"{f}: retail's blueprints set it and src/ never reads it; read it, "
                       "or list it in tools/blueprint_fields_baseline.txt")
    for f in baseline:
        if f not in known:
            out.append(f"{f}: in the baseline but not in tools/blueprint_fields.txt")
        elif f in read:
            out.append(f"{f}: src/ reads it now; take it off tools/blueprint_fields_baseline.txt")
    return out


def check(root: Path) -> list[str]:
    tools = root / "tools"
    return problems_of(read_names(tools / "blueprint_fields.txt"),
                       read_names(tools / "blueprint_fields_baseline.txt"), literals(root))


def self_test() -> int:
    exe = b"\x00GuardScanRadius\x00MaxSpeed\x00Description\x00"
    bps = ["UnitBlueprint {\n  AI = { GuardScanRadius = 25, },\n  # Hidden = 1\n"
           "  MaxSpeed = 2,\n  Description = 'x',\n}"]
    luas = ["local d = bp.Description -- bp.MaxSpeed"]
    got = fields_from(exe, bps, luas)
    if got != ["GuardScanRadius", "MaxSpeed"]:
        print(f"self-test: fields_from gave {got}")
        return 1
    cases = [
        ({"GuardScanRadius", "MaxSpeed"}, [], 0),
        ({"MaxSpeed"}, [], 1),
        ({"MaxSpeed"}, ["GuardScanRadius"], 0),
        ({"GuardScanRadius", "MaxSpeed"}, ["GuardScanRadius"], 1),
        ({"GuardScanRadius", "MaxSpeed"}, ["Nope"], 1),
    ]
    for read, baseline, n in cases:
        if len(problems_of(got, baseline, read)) != n:
            print(f"self-test: read {sorted(read)}, baseline {baseline}: want {n} problem(s)")
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
    print(f"blueprint fields: {len(read_names(Path(argv[0]) / 'tools' / 'blueprint_fields.txt'))} "
          "engine-only fields, each read or in the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
