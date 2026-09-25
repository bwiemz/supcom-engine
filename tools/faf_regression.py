#!/usr/bin/env python3
"""Play a four-AI game on FAF's current game Lua and tally what goes wrong.

The engine was first built against FAForever's data and now runs retail FA's
by default; this is the FAF regression run. It needs no FAF client: FAF's
game code (lua, units, effects, ...) is the FAForever/fa repository, cloned
shallow into a cache folder, and mounted over retail FA's art and sounds the
way FAF's own init_faf.lua mounts its built packages (FAF first, then only the
retail archives FAF allows).

    tools/faf_regression.py --engine build/linux-release/opensupcom \\
        --fa-path "<Steam>/Supreme Commander Forged Alliance" [--ticks 3000]

Prints the engine's exit status and each distinct warning or error with its
count, most frequent first. Exits non-zero when the engine crashed, or with
--fail-on-errors when any Lua error was reported.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

FAF_REPO = "https://github.com/FAForever/fa.git"

# FAF's allowedAssetsScd (init.lua): what it takes from retail FA. The rest
# (lua, schook, moholua, mohodata) is in its repository.
RETAIL_ARCHIVES = [
    "units.scd",
    "textures.scd",
    "skins.scd",
    "props.scd",
    "projectiles.scd",
    "objects.scd",
    "mods.scd",
    "meshes.scd",
    "loc_us.scd",
    "env.scd",
    "effects.scd",
]

INIT_TEMPLATE = """-- FAF's game Lua ({repo}) over retail FA's data, for the engine's FAF
-- regression run (tools/faf_regression.py). FAF first: the first mount wins.
path = {{}}
local function mount(dir, mountpoint)
    table.insert(path, {{ dir = dir, mountpoint = mountpoint }})
end
mount('{repo}', '/')
for _, name in {{ {archives} }} do
    mount(fa_path .. '/gamedata/' .. name, '/') -- a missing one is skipped
end
mount(fa_path .. '/movies', '/movies')
mount(fa_path .. '/sounds', '/sounds')
mount(fa_path .. '/maps', '/maps')
mount(fa_path .. '/fonts', '/fonts')
hook = {{ '/schook' }}
protocols = {{ 'http', 'https', 'mailto', 'ventrilo', 'teamspeak', 'daap', 'im' }}
"""


def ensure_repo(cache: Path, branch: str) -> Path:
    repo = cache / "fa"
    if not (repo / "lua").is_dir():
        cache.mkdir(parents=True, exist_ok=True)
        _ = subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", branch, FAF_REPO, str(repo)],
            check=True,
        )
    return repo


def write_init(cache: Path, repo: Path) -> Path:
    init = cache / "init_faf_regression.lua"
    archives = ", ".join(f"'{a}'" for a in RETAIL_ARCHIVES)
    _ = init.write_text(INIT_TEMPLATE.format(repo=repo.as_posix(), archives=archives))
    return init


def tally(log: str) -> Counter[str]:
    """Each distinct warning or error, numbers masked so repeats group."""
    counts: Counter[str] = Counter()
    for line in log.splitlines():
        m = re.search(r"\[(warning|error)\] (.*)", line)
        if not m or "stack traceback" in m.group(2):
            continue
        counts[f"{m.group(1)}: {re.sub(r'[0-9]+', 'N', m.group(2))[:160]}"] += 1
    return counts


class Args(argparse.Namespace):
    # The defaults live here, not in add_argument: argparse leaves an
    # attribute the namespace already has alone.
    engine: str = ""
    fa_path: str = ""
    cache: str = str(Path.home() / ".cache" / "osc-faf")
    branch: str = "develop"
    map: str = "/maps/SCMP_009/SCMP_009_scenario.lua"
    ticks: int = 3000
    seed: str = "4242"
    log: str | None = None
    fail_on_errors: bool = False


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    _ = ap.add_argument("--engine", required=True, help="the opensupcom to run")
    _ = ap.add_argument("--fa-path", required=True, help="retail FA's install folder")
    _ = ap.add_argument("--cache", help="where FAF's repository is cloned")
    _ = ap.add_argument("--branch", help="FAF's branch (default: develop)")
    _ = ap.add_argument("--map")
    _ = ap.add_argument("--ticks", type=int)
    _ = ap.add_argument("--seed")
    _ = ap.add_argument("--log", help="keep the engine's log here")
    _ = ap.add_argument("--fail-on-errors", action="store_true")
    args = ap.parse_args(namespace=Args())
    cache = Path(args.cache)
    log_path = Path(args.log) if args.log else None

    repo = ensure_repo(cache, args.branch)
    init = write_init(cache, repo)
    run = subprocess.run(
        [
            args.engine,
            "--init",
            str(init),
            "--fa-path",
            args.fa_path,
            "--map",
            args.map,
            "--ai-skirmish",
            "--ai-armies",
            "4",
            "--ticks",
            str(args.ticks),
            "--seed",
            args.seed,
        ],
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    log = run.stdout + run.stderr
    if log_path:
        _ = log_path.write_text(log)
    crashed = run.returncode < 0 or run.returncode >= 128 or "OpenSupCom crashed" in log
    print(f"engine exit {run.returncode}{' (crashed)' if crashed else ''}")
    progress: list[tuple[str, str]] = re.findall(r"Tick (\d+): [^|]*\| (\d+) units alive", log)
    if progress:
        print(f"reached tick {progress[-1][0]} with {progress[-1][1]} units alive")
    counts = tally(log)
    for message, n in counts.most_common():
        print(f"{n:6d}  {message}")
    lua_errors = sum(n for m, n in counts.items() if "error" in m.lower())
    if crashed:
        return 2
    return 1 if args.fail_on_errors and lua_errors else 0


if __name__ == "__main__":
    sys.exit(main())
