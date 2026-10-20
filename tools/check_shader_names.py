#!/usr/bin/env python3
"""Fail on a retail ShaderName the renderer has no technique for.

mesh_technique() in src/renderer/mesh_cache.cpp draws a ShaderName it does not
know as a Unit. That drew the UEF nuke's dome as an opaque sphere.

tools/shader_names.txt lists the ShaderName values in retail's .bp files.
tools/shader_names_baseline.txt lists those, after the engine's remaps, with no
technique yet. The check fails on one missing from the baseline, and on a
baseline entry that has a technique now or that retail no longer sets.

Usage: check_shader_names.py <repo root>          (exit 1 on a problem)
       check_shader_names.py --extract <FA dir> > tools/shader_names.txt
       check_shader_names.py --installed <repo root> <opensupcom>
           (the same check on the installed game's .bp files, in the install
           `opensupcom --print-install` names; exit 77, a skip, when there is none)
       check_shader_names.py --self-test
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

SHADER = re.compile(r"(?i)\bShaderName\s*=\s*(['\"])([^'\"\n]*)\1")
REMAP = re.compile(r'\{"(\w+)",\s*"(\w+)"\}')
TECHNIQUE = re.compile(r'shader_name == "(\w+)"')
# Unit is the default technique; Wreckage is drawn by its own path.
OWN_PATH = {"Unit", "Wreckage"}


def shader_names(text: str) -> set[str]:
    return {m.group(2) for m in SHADER.finditer(text)} - {""}


def engine_shaders(source: str) -> tuple[dict[str, str], set[str]]:
    """The remaps resolve_shader_name applies, and the names mesh_technique knows."""
    remaps_part, _, rest = source.partition("MeshTechnique mesh_technique(")
    technique_part = rest.partition("\n}\n")[0]
    remaps = dict(REMAP.findall(remaps_part.partition("kRemaps")[2]))
    return remaps, set(TECHNIQUE.findall(technique_part)) | OWN_PATH


def without_technique(names: list[str], remaps: dict[str, str], known: set[str]) -> set[str]:
    resolved = {remaps.get(n, n) for n in names}
    return resolved - known


def names_in(fa: Path) -> set[str]:
    names: set[str] = set()
    for scd in sorted((fa / "gamedata").glob("*.scd")):
        with zipfile.ZipFile(scd) as z:
            for name in z.namelist():
                if name.lower().endswith(".bp"):
                    names |= shader_names(z.read(name).decode("latin-1"))
    return names


def extract(fa: Path) -> int:
    names = names_in(fa)
    if not names:
        print(f"{fa}: no ShaderName in any gamedata/*.scd blueprint; is it a game install?",
              file=sys.stderr)
        return 1
    print("# Retail ShaderName values: tools/check_shader_names.py --extract")
    for n in sorted(names):
        print(n)
    return 0


def read_names(path: Path) -> list[str]:
    out = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def problems_of(missing: set[str], baseline: list[str]) -> list[str]:
    out = []
    for n in sorted(missing - set(baseline)):
        out.append(f"{n}: retail sets this ShaderName and the renderer has no technique for it; "
                   "add one, or list it in tools/shader_names_baseline.txt")
    for n in baseline:
        if n not in missing:
            out.append(f"{n}: has a technique now, or retail no longer sets it; "
                       "take it off tools/shader_names_baseline.txt")
    return out


def check(root: Path, names: list[str] | None = None) -> tuple[list[str], int]:
    tools = root / "tools"
    names = read_names(tools / "shader_names.txt") if names is None else names
    remaps, known = engine_shaders((root / "src/renderer/mesh_cache.cpp").read_text())
    missing = without_technique(names, remaps, known)
    return problems_of(missing, read_names(tools / "shader_names_baseline.txt")), len(missing)


def install_of(print_install: str) -> Path | None:
    """The FA directory in `opensupcom --print-install`'s output, if it found one."""
    m = re.search(r"^fa_path=(.+)$", print_install, re.M)
    return Path(m.group(1)) if m else None


def check_installed(root: Path, engine: str) -> int:
    run = subprocess.run([engine, "--print-install"], capture_output=True, text=True)
    fa = install_of(run.stdout) if run.returncode == 0 else None
    names = names_in(fa) if fa and (fa / "gamedata").is_dir() else set()
    if not names:
        print(f"{engine} --print-install found no installed game; skipping")
        return 77
    return report(*check(root, sorted(names)), "installed")


def report(problems: list[str], n: int, what: str) -> int:
    for p in problems:
        print(p)
    if problems:
        return 1
    print(f"shader names: {n} {what} ShaderName values without a technique, all in the baseline")
    return 0


def self_test() -> int:
    got = shader_names("Mesh { LODs = { { ShaderName = 'Aeon' }, { shadername = \"Flat\" },"
                       " { ShaderName = '' } } }")
    if got != {"Aeon", "Flat"}:
        print(f"self-test: shader_names gave {sorted(got)}")
        return 1
    source = ('kRemaps[] = {\n {"TMeshNoLighting", "Flat"},\n};\n'
              'MeshTechnique mesh_technique(x) {\n if (shader_name == "Aeon") return 1;\n'
              ' return Unit;\n}')
    later = source + '\nvoid other() {\n if (shader_name == "Helper") return;\n}\n'
    remaps, known = engine_shaders(later)
    if remaps != {"TMeshNoLighting": "Flat"} or known != {"Aeon", "Unit", "Wreckage"}:
        print(f"self-test: engine_shaders gave {remaps}, {sorted(known)}")
        return 1
    missing = without_technique(["Aeon", "TMeshNoLighting", "Unit", "Brand"], remaps, known)
    if missing != {"Flat", "Brand"}:
        print(f"self-test: without_technique gave {sorted(missing)}")
        return 1
    cases = [(["Flat", "Brand"], 0), (["Flat"], 1), (["Flat", "Brand", "Aeon"], 1)]
    for baseline, n in cases:
        if len(problems_of(missing, baseline)) != n:
            print(f"self-test: baseline {baseline}: want {n} problem(s)")
            return 1
    if install_of("source=steam\nfa_path=/g/FA\ninit_file=/g/FA/bin/x.lua\n") != Path("/g/FA") \
            or install_of("searched steam: /s\n") is not None:
        print("self-test: install_of must read fa_path= and nothing else")
        return 1
    with tempfile.TemporaryDirectory() as empty:
        if extract(Path(empty)) != 1:
            print("self-test: --extract on a directory without game data must fail")
            return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) == 2 and argv[0] == "--extract":
        return extract(Path(argv[1]))
    if len(argv) == 3 and argv[0] == "--installed":
        return check_installed(Path(argv[1]), argv[2])
    if len(argv) != 1:
        print(__doc__)
        return 2
    return report(*check(Path(argv[0])), "retail")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
