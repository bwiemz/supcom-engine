#!/usr/bin/env python3
"""Play games on FAF's game Lua and tally what goes wrong.

The engine was first built against FAForever's data and now runs retail FA's
by default; this is the FAF regression run. It needs no FAF client: FAF's
game code (lua, units, effects, ...) is the FAForever/fa repository, cloned
shallow into a cache folder, and mounted over retail FA's art and sounds.

Two ways to mount it:

- default: an init of this tool's own mounts the repository folder first,
  then the retail archives FAF allows (quick, for a working checkout);
- --packaged: a FAF data folder as the FAF client installs it. The
  repository's own bin/init_faf.lua runs, over gamedata/<name>.nx2 packages
  made from the folders it allows (as FAF's deployment packs them), and the
  engine is started with --faf-data. This is what FAF players run.

Use --branch deploy/faf for the release FAF players have, or develop.

    tools/faf_regression.py --engine build/linux-release/opensupcom \\
        --fa-path "<Steam>/Supreme Commander Forged Alliance" [--ticks 3000]
    tools/faf_regression.py ... --packaged --branch deploy/faf --suite long

One game prints the engine's exit status and each distinct warning or error
with its count, most frequent first. --suite long plays the long set (five
skirmishes of 2 to 8 AIs, up to 18,000 ticks, and FA's six operations for
6,000 ticks each) and prints a line a game. Exits non-zero when the engine
crashed or hung (--timeout, 30 minutes a game by default), or with
--fail-on-errors when any Lua error was reported.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
import zipfile
from collections import Counter
from dataclasses import dataclass
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


@dataclass(frozen=True)
class Game:
    name: str
    args: tuple[str, ...]


def skirmish(name: str, map_id: str, armies: int, ticks: int, seed: int) -> Game:
    return Game(
        name,
        (
            "--map",
            f"/maps/{map_id}/{map_id}_scenario.lua",
            "--ai-skirmish",
            "--ai-armies",
            str(armies),
            "--ticks",
            str(ticks),
            "--seed",
            str(seed),
        ),
    )


# The long set: retail AIs on five maps, and FA's six operations headless.
LONG_SUITE = [
    skirmish("scmp009_4ai", "SCMP_009", 4, 18000, 4242),
    skirmish("scmp001_8ai", "SCMP_001", 8, 12000, 7),
    skirmish("scmp016_2ai", "SCMP_016", 2, 18000, 11),
    skirmish("scmp026_4ai", "SCMP_026", 4, 12000, 13),
    skirmish("scmp028_8ai", "SCMP_028", 8, 9000, 17),
] + [
    Game(f"x1ca_{op}", ("--map", f"/maps/X1CA_{op}/X1CA_{op}_scenario.lua", "--ticks", "6000"))
    for op in ("001", "002", "003", "004", "005", "006")
]


def ensure_repo(cache: Path, branch: str) -> Path:
    # One clone per branch: deploy/faf and develop differ.
    repo = cache / ("fa" if branch == "develop" else f"fa-{branch.replace('/', '-')}")
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


def allowed_packages(init_faf: str) -> list[str]:
    """The packages init_faf.lua mounts: `allowedAssetsNxy["lua.nx2"] = true`
    (a commented-out line, like schook's, doesn't count)."""
    names: list[str] = []
    for line in init_faf.splitlines():
        m = re.match(r'\s*allowedAssetsNxy\["([^"]+)\.nx2"\]\s*=\s*true', line)
        if m and m.group(1) not in names:
            names.append(m.group(1))
    return names


def pack_folder(repo: Path, folder: str, out: Path) -> int:
    """`folder` as FAF's deployment packs it: a zip of its files, each under
    `<folder>/`. Returns how many files it holds."""
    n = 0
    tmp = out.with_suffix(".tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted((repo / folder).rglob("*")):
            if path.is_file():
                z.write(path, path.relative_to(repo).as_posix())
                n += 1
    _ = tmp.replace(out)
    return n


def make_faf_data(repo: Path, fa_path: str, out: Path) -> Path:
    """A FAF data folder as the client installs it: bin/init_faf.lua,
    fa_path.lua, and gamedata/<name>.nx2 for each package init_faf.lua
    allows that the repository has. Rebuilt when the checkout changes."""
    head = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
    ).stdout.strip()
    stamp = out / "built-from"
    if stamp.is_file() and stamp.read_text() == f"{head}\n{fa_path}\n":
        return out
    init_faf = (repo / "init_faf.lua").read_text(errors="replace")
    (out / "bin").mkdir(parents=True, exist_ok=True)
    (out / "gamedata").mkdir(exist_ok=True)
    _ = (out / "bin" / "init_faf.lua").write_text(init_faf)
    # A Lua string: init_faf.lua runs `dofile(InitFileDir .. '/../fa_path.lua')`.
    _ = (out / "fa_path.lua").write_text(f"fa_path = {lua_string(fa_path)}\n")
    for name in allowed_packages(init_faf):
        if (repo / name).is_dir():
            files = pack_folder(repo, name, out / "gamedata" / f"{name}.nx2")
            print(f"packed {name}.nx2 ({files} files)", file=sys.stderr)
    _ = stamp.write_text(f"{head}\n{fa_path}\n")
    return out


def lua_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def tally(log: str) -> Counter[str]:
    """Each distinct warning or error, numbers masked so repeats group."""
    counts: Counter[str] = Counter()
    for line in log.splitlines():
        m = re.search(r"\[(warning|error)\] (.*)", line)
        if not m or "stack traceback" in m.group(2):
            continue
        counts[f"{m.group(1)}: {re.sub(r'[0-9]+', 'N', m.group(2))[:160]}"] += 1
    return counts


@dataclass
class Outcome:
    exit_code: int
    crashed: bool
    hung: bool
    seconds: float
    thread_errors: int
    script_errors: int
    error_lines: int
    end: str
    counts: Counter[str]


def play(base: list[str], game: Game, log_path: Path | None, timeout: float) -> Outcome:
    """One game; one that outlives `timeout` seconds is killed and counts
    as hung (a script looping forever never ends on its own)."""
    start = time.monotonic()
    hung = False
    try:
        run = subprocess.run(
            [*base, *game.args],
            capture_output=True,
            text=True,
            errors="replace",
            check=False,
            timeout=timeout,
        )
        exit_code = run.returncode
        log = run.stdout + run.stderr
    except subprocess.TimeoutExpired as e:
        hung = True
        exit_code = -1
        log = "".join(
            part.decode(errors="replace") if isinstance(part, bytes) else (part or "")
            for part in (e.stdout, e.stderr)
        )
    seconds = time.monotonic() - start
    if log_path:
        _ = log_path.write_text(log)
    ends: list[str] = re.findall(r"Sim: (\d+ armies, \d+ entities, .*? ticks)", log)
    return Outcome(
        exit_code=exit_code,
        crashed=not hung and (exit_code < 0 or exit_code >= 128 or "OpenSupCom crashed" in log),
        hung=hung,
        seconds=seconds,
        thread_errors=log.count("Thread error"),
        script_errors=len(re.findall(r"\[warning\].* error: ", log)),
        error_lines=log.count("[error]"),
        end=ends[-1] if ends else "",
        counts=tally(log),
    )


class Args(argparse.Namespace):
    # The defaults live here, not in add_argument: argparse leaves an
    # attribute the namespace already has alone.
    engine: str = ""
    fa_path: str = ""
    cache: str = str(Path.home() / ".cache" / "osc-faf")
    branch: str = "develop"
    packaged: bool = False
    suite: str | None = None
    map: str = "/maps/SCMP_009/SCMP_009_scenario.lua"
    ticks: int = 3000
    seed: str = "4242"
    log: str | None = None
    timeout: float = 1800.0
    fail_on_errors: bool = False


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    _ = ap.add_argument("--engine", required=True, help="the opensupcom to run")
    _ = ap.add_argument("--fa-path", required=True, help="retail FA's install folder")
    _ = ap.add_argument("--cache", help="where FAF's repository is cloned")
    _ = ap.add_argument("--branch", help="FAF's branch (default: develop; deploy/faf: the release)")
    _ = ap.add_argument(
        "--packaged",
        action="store_true",
        help="run FAF's own init_faf.lua over .nx2 packages, as the FAF client installs it",
    )
    _ = ap.add_argument("--suite", choices=["long"], help="play the long set instead of one game")
    _ = ap.add_argument("--map")
    _ = ap.add_argument("--ticks", type=int)
    _ = ap.add_argument("--seed")
    _ = ap.add_argument("--log", help="keep the engine's log here (a folder with --suite)")
    _ = ap.add_argument(
        "--timeout", type=float, help="seconds before a game counts as hung (default 1800)"
    )
    _ = ap.add_argument("--fail-on-errors", action="store_true")
    args = ap.parse_args(namespace=Args())
    cache = Path(args.cache)

    repo = ensure_repo(cache, args.branch)
    if args.packaged:
        data = make_faf_data(repo, args.fa_path, cache / f"data-{repo.name}")
        base = [args.engine, "--faf-data", str(data), "--fa-path", args.fa_path]
    else:
        base = [args.engine, "--init", str(write_init(cache, repo)), "--fa-path", args.fa_path]

    if args.suite:
        log_dir = Path(args.log) if args.log else None
        if log_dir:
            log_dir.mkdir(parents=True, exist_ok=True)
        crashed = errors = 0
        for game in LONG_SUITE:
            o = play(base, game, log_dir / f"{game.name}.log" if log_dir else None, args.timeout)
            crashed += o.crashed or o.hung
            errors += o.thread_errors + o.script_errors + o.error_lines
            print(
                f"{game.name:12} exit={o.exit_code:<3}{' CRASHED' if o.crashed else ''}"
                + f"{' HUNG' if o.hung else ''} "
                + f"{o.seconds:6.0f}s thread_errors={o.thread_errors} "
                + f"script_errors={o.script_errors} error_lines={o.error_lines}  {o.end}",
                flush=True,
            )
        if crashed:
            return 2
        return 1 if args.fail_on_errors and errors else 0

    game = Game(
        "game",
        (
            "--map",
            args.map,
            "--ai-skirmish",
            "--ai-armies",
            "4",
            "--ticks",
            str(args.ticks),
            "--seed",
            args.seed,
        ),
    )
    o = play(base, game, Path(args.log) if args.log else None, args.timeout)
    print(
        f"engine exit {o.exit_code}{' (crashed)' if o.crashed else ''}"
        + f"{' (hung: killed after the timeout)' if o.hung else ''}"
    )
    if o.end:
        print(f"reached: {o.end}")
    for message, n in o.counts.most_common():
        print(f"{n:6d}  {message}")
    lua_errors = sum(n for m, n in o.counts.items() if "error" in m.lower())
    if o.crashed or o.hung:
        return 2
    return 1 if args.fail_on_errors and lua_errors else 0


if __name__ == "__main__":
    sys.exit(main())
