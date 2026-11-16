"""A skirmish with mods, as a player would have them (M221b, M221c).

CTest: data.mods_flow. The player's mods folder (retail's init file mounts
each entry of `<Documents>/My Games/Gas Powered Games/Supreme Commander
Forged Alliance/mods` at /mods/<name>; the Documents folder here is a
temporary one, through XDG_DOCUMENTS_DIR) holds two test mods:

- a game mod, a folder: a blueprint of its own that merges into the UEF
  ACU's, and hooks of /lua/simInit.lua and /lua/userInit.lua that leave a
  probe;
- a UI-only mod, a .zip: hooks of /lua/userInit.lua (which runs before UI
  mods join the session, so must not run) and /lua/UserSync.lua (which
  must);
- a broken mod: its mod_info.lua doesn't compile, so the game leaves it out;
  it hooks a misspelt /lua/simInt.lua, and its unit's .bp doesn't compile.

1. The player's preferences select both and retail's Resource Rich (from
   gamedata/mods.scd), whose ModBlueprints hook doubles every unit's
   production. `--mods-flow-test` launches a skirmish from the front end as
   retail's lobby does, with GameMods = Mods.GetGameMods(), and reports each
   Lua state's mods at tick 20. The sim must have the game mods only, sorted
   by uid as retail's mods.lua sorts them; the game's UI the game mods, then
   the UI mod; both the merged blueprint with doubled production; and each
   its hooks' probes.
2. The game's recording (LastGame) must replay headless with the same mods:
   every tick's checksum as recorded, and the game mod's hook run.
3. Watched in the game (`--watch`), it must give the same report.
4. With nothing selected, `--mods-flow-lobby` clicks through retail's front
   end, skirmish lobby and mod manager as a player would, picking Resource
   Rich alone: the game must have it, and only it, and the player's
   selection must be saved.
5. `--check-mods --json` checks every installed mod: the broken one has its
   three errors, each in its file (and the first two on their lines), and
   the two good ones are clean; it exits 1. Named by its folder on disk, the
   broken mod alone is checked, and the text report names its errors;
   `--list-mods --json` lists the three.

Any script error fails a run.

Usage:
    mods_flow.py <opensupcom>

Exits 77 (skipped) when the game has no data.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast

TIMEOUT_SECONDS = 600
SKIPPED = 77

RESOURCE_RICH = "74A9EAB2-E851-11DB-A1F1-F2C755D89593"
GAME_MOD = "osc-mods-flow-game"
UI_MOD = "osc-mods-flow-ui"
BROKEN_MOD = "/mods/oscmodsflowbroken"  # its uid is unreadable: named by location
MODS_DIR = Path("My Games/Gas Powered Games/Supreme Commander Forged Alliance/mods")

# The UEF ACU's production before Resource Rich doubles it
ACU_MASS = 1
ACU_ENERGY = 20


def probe(text: str) -> str:
    """A hook that appends `text` to the state's probe global."""
    return f"OscModsFlowProbe = (rawget(_G, 'OscModsFlowProbe') or '') .. '{text};'\n"


def write_mods(documents: Path) -> None:
    mods = documents / MODS_DIR
    game = mods / "OscModsFlowGame"
    (game / "hook/lua").mkdir(parents=True)
    (game / "units/uel0001").mkdir(parents=True)
    _ = (game / "mod_info.lua").write_text(
        f'name = "OSC mods flow: game"\nuid = "{GAME_MOD}"\nversion = 1\n'
        + 'description = "A test game mod"\nauthor = "OpenSupCom"\nui_only = false\n'
    )
    _ = (game / "hook/lua/simInit.lua").write_text(probe("game:sim"))
    _ = (game / "hook/lua/userInit.lua").write_text(probe("game:user"))
    # Named as the ACU's own file, so Blueprints.lua gives it the ACU's id;
    # Merge keeps the rest of the ACU.
    _ = (game / "units/uel0001/uel0001_unit.bp").write_text(
        "UnitBlueprint {\n    Merge = true,\n    General = { UnitName = 'ModsFlow' },\n}\n"
    )

    # The UI mod packed, as a mod downloaded from a vault is
    with zipfile.ZipFile(mods / "OscModsFlowUi.zip", "w") as ui:
        ui.writestr(
            "mod_info.lua",
            f'name = "OSC mods flow: UI"\nuid = "{UI_MOD}"\nversion = 1\n'
            + 'description = "A test UI mod"\nauthor = "OpenSupCom"\nui_only = true\n',
        )
        ui.writestr("hook/lua/userInit.lua", probe("ui:user"))
        ui.writestr("hook/lua/UserSync.lua", probe("ui:sync"))

    # A mod with a defect of each kind --check-mods reports
    broken = mods / "OscModsFlowBroken"
    (broken / "hook/lua").mkdir(parents=True)
    (broken / "units/xyz0001").mkdir(parents=True)
    _ = (broken / "mod_info.lua").write_text(
        'name = "OSC mods flow: broken"\nuid = "osc-mods-flow-broken"\nversion is 1\n'
    )
    _ = (broken / "hook/lua/simInt.lua").write_text(probe("broken:sim"))
    _ = (broken / "units/xyz0001/xyz0001_unit.bp").write_text(
        "UnitBlueprint {\n    General = { UnitName = 'Broken' }\n    Display = {},\n}\n"
    )


@dataclass
class Diagnostic:
    """A finding --check-mods must report for the broken mod."""

    check: str
    file: str
    line: int  # 0: any
    words: str  # in its message


BROKEN_FINDINGS = [
    Diagnostic("mod_info", f"{BROKEN_MOD}/mod_info.lua", 3, "doesn't run"),
    Diagnostic(
        "hook_target", f"{BROKEN_MOD}/hook/lua/simint.lua", 0, "did you mean /lua/siminit.lua"
    ),
    Diagnostic(
        "blueprint_compile", f"{BROKEN_MOD}/units/xyz0001/xyz0001_unit.bp", 3, "doesn't compile"
    ),
]


def check_mods_report(output: str) -> tuple[dict[str, dict[str, object]], list[str]]:
    """--check-mods --json's report: the mods by location, or the problem reading it."""
    try:
        report = cast(object, json.loads(output))
    except json.JSONDecodeError as e:
        return {}, [f"check-mods: not JSON ({e}): {output[:200]!r}"]
    listed = cast(dict[str, object], report).get("mods") if isinstance(report, dict) else None
    if not isinstance(listed, list):
        return {}, ["check-mods: no list of mods"]
    mods: dict[str, dict[str, object]] = {}
    for mod in cast(list[dict[str, object]], listed):
        mods[str(mod.get("location"))] = mod
    return mods, []


def check_mods_problems(mods: dict[str, dict[str, object]]) -> list[str]:
    """What --check-mods got wrong about the three test mods."""
    problems: list[str] = []
    for location in ("/mods/oscmodsflowgame", "/mods/oscmodsflowui"):
        mod = mods.get(location)
        if mod is None:
            problems.append(f"check-mods: {location} not checked")
        elif mod.get("findings"):
            problems.append(f"check-mods: {location} isn't clean: {mod.get('findings')}")
    broken = mods.get(BROKEN_MOD)
    if broken is None:
        return [*problems, f"check-mods: {BROKEN_MOD} not checked"]
    findings = cast(list[dict[str, object]], broken.get("findings") or [])
    for want in BROKEN_FINDINGS:
        if not any(
            f.get("check") == want.check
            and f.get("file") == want.file
            and (want.line == 0 or f.get("line") == want.line)
            and f.get("severity") == "error"
            and want.words in str(f.get("message"))
            for f in findings
        ):
            problems.append(
                f"check-mods: no {want.check} error in {want.file}"
                + (f":{want.line}" if want.line else "")
                + f" saying {want.words!r}: {findings}"
            )
    if len(findings) != len(BROKEN_FINDINGS):
        problems.append(f"check-mods: {len(findings)} findings for {BROKEN_MOD}: {findings}")
    return problems


def report(output: str, state: str) -> dict[str, str]:
    """The flow's report for `state` ('sim' or 'ui'), as key=value pairs."""
    match = re.search(rf"mods-flow: {state}: (.*)", output)
    if not match:
        return {}
    return dict(re.findall(r"(\w+)=(\S*)", match.group(1)))


@dataclass
class Expected:
    """What a run's report must say, per Lua state ('sim', 'ui')."""

    mods: dict[str, list[str]]
    name: str  # the UEF ACU's UnitName
    wanted: dict[str, list[str]]  # probes of hooks that must run
    unwanted: dict[str, list[str]] = field(default_factory=dict)  # ...and must not
    user_sync_runs: int = 0  # the UI mod's UserSync.lua hook
    selected: list[str] = field(default_factory=list)  # the player's, in the UI


def check(output: str, run_name: str, want: Expected) -> list[str]:
    """What is wrong with a run's report (each problem named for the run)."""
    problems: list[str] = []
    for state in ("sim", "ui"):
        got = report(output, state)
        if not got:
            problems.append(f"{state}: no report")
            continue
        mods = ",".join(want.mods[state])
        if got.get("mods") != mods:
            problems.append(f"{state}: mods {got.get('mods')}, expected {mods}")
        if got.get("mass") != str(2 * ACU_MASS) or got.get("energy") != str(2 * ACU_ENERGY):
            problems.append(
                f"{state}: the ACU makes {got.get('mass')} mass and {got.get('energy')} energy,"
                + f" expected Resource Rich's {2 * ACU_MASS} and {2 * ACU_ENERGY}"
            )
        if got.get("name") != want.name:
            problems.append(f"{state}: the ACU's UnitName is {got.get('name')}, not {want.name}")
        probes = got.get("probe", "")
        problems += [
            f"{state}: no {p} hook ran (probe {probes})"
            for p in want.wanted.get(state, [])
            if p not in probes
        ]
        problems += [
            f"{state}: the {p} hook ran (probe {probes})"
            for p in want.unwanted.get(state, [])
            if p in probes
        ]
        if state == "ui":
            if probes.count("ui:sync") != want.user_sync_runs:
                problems.append(
                    f"ui: UserSync.lua's hook ran {probes.count('ui:sync')} times,"
                    + f" not {want.user_sync_runs}"
                )
            selected = ",".join(want.selected)
            if got.get("selected") != selected:
                problems.append(f"ui: selected {got.get('selected')}, expected {selected}")
    return [f"{run_name}: {problem}" for problem in problems]


# Runs 1-3: the three mods selected. Retail's SessionInit.lua loads
# UserSync.lua, and so does its /schook hook: the world UI must not load it
# a third time.
GAME_MODS = sorted([RESOURCE_RICH, GAME_MOD])
ALL_SELECTED = Expected(
    mods={"sim": GAME_MODS, "ui": [*GAME_MODS, UI_MOD]},
    name="ModsFlow",
    wanted={"sim": ["game:sim"], "ui": ["game:user", "ui:sync"]},
    unwanted={"sim": ["game:user", "ui:user", "ui:sync"], "ui": ["game:sim", "ui:user"]},
    user_sync_runs=2,
    selected=sorted([RESOURCE_RICH, GAME_MOD, UI_MOD]),
)
# Run 4: Resource Rich alone, picked in the mod manager (retail's ACU has
# no UnitName)
PICKED_IN_LOBBY = Expected(
    mods={"sim": [RESOURCE_RICH], "ui": [RESOURCE_RICH]},
    name="nil",
    wanted={},
    unwanted={"sim": ["game:", "ui:"], "ui": ["game:", "ui:"]},
    selected=[RESOURCE_RICH],
)


def run(args: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        capture_output=True,
        text=True,
        errors="replace",  # retail's mod_info files are ISO-8859
        timeout=TIMEOUT_SECONDS,
        check=False,
        env=env,
    )


def show(output: str, run_name: str) -> None:
    for line in output.splitlines():
        if "mods-flow:" in line or "FAIL" in line:
            print(f"{run_name}: {line}")


def main(argv: list[str]) -> int:
    if len(argv) < 1:
        print(__doc__, file=sys.stderr)
        return 2
    exe = argv[0]
    with tempfile.TemporaryDirectory(prefix="osc-mods-flow-") as tmp:
        documents = Path(tmp) / "Documents"
        write_mods(documents)
        prefs = Path(tmp) / "Game.prefs"
        _ = prefs.write_text(
            "active_mods = {\n"
            + f"    ['{RESOURCE_RICH}'] = true,\n"
            + f"    ['{GAME_MOD}'] = true,\n"
            + f"    ['{UI_MOD}'] = true,\n"
            + "}\n"
        )
        env = {**os.environ, "XDG_DOCUMENTS_DIR": str(documents)}
        user = Path(tmp) / "user"
        game = run([exe, "--prefs", str(prefs), "--user-dir", str(user), "--mods-flow-test"], env)
        if game.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        output = game.stdout + game.stderr
        show(output, "game")
        problems = check(output, "game", ALL_SELECTED)
        if game.returncode != 0:
            problems.append(f"game: failed, exit {game.returncode}")

        recording = user / "replays" / "Player" / "LastGame.oscreplay"
        # A player's game is kept under a name of its own too (when it ended
        # and its map), with its statistics beside it: the game before last
        # is still there to watch and to study.
        kept = [f for f in recording.parent.glob("*.oscreplay") if f.name != recording.name]
        if len(kept) != 1:
            problems.append(f"game: kept {len(kept)} recordings under names of their own, not 1")
        else:
            if kept[0].read_bytes() != recording.read_bytes():
                problems.append("game: the kept recording is not the game's")
            stats = kept[0].with_suffix(".jsonl")
            lines = stats.read_text().splitlines() if stats.exists() else []
            kinds = [
                line.split('"record":"')[1].split('"')[0] for line in lines if '"record":"' in line
            ]
            if not kinds or kinds[0] != "match" or kinds[-1] != "end":
                problems.append(
                    f"game: its statistics are not a match to its end: {kinds[:3]}..{kinds[-1:]}"
                )
        if not recording.exists():
            problems.append("game: left no recording")
        else:
            replay = run([exe, "--replay", str(recording)], env)
            replayed = replay.stdout + replay.stderr
            if replay.returncode != 0:
                problems.append(f"replay: failed or diverged, exit {replay.returncode}")
            if "Hooked /lua/siminit.lua with /mods/oscmodsflowgame/" not in replayed:
                problems.append("replay: ran without the game's mods")
            watch = run(
                [
                    exe,
                    "--prefs",
                    str(prefs),
                    "--user-dir",
                    str(user),
                    "--mods-flow-test",
                    "--watch",
                    str(recording),
                ],
                env,
            )
            watched = watch.stdout + watch.stderr
            show(watched, "watched")
            problems += check(watched, "watched", ALL_SELECTED)
            if watch.returncode != 0:
                problems.append(f"watched: failed, exit {watch.returncode}")

        # Nothing selected: the player picks in the lobby's mod manager
        _ = prefs.write_text("")
        lobby = run(
            [
                exe,
                "--prefs",
                str(prefs),
                "--user-dir",
                str(Path(tmp) / "lobby"),
                "--mods-flow-test",
                "--mods-flow-lobby",
            ],
            env,
        )
        clicked = lobby.stdout + lobby.stderr
        show(clicked, "lobby")
        problems += check(clicked, "lobby", PICKED_IN_LOBBY)
        if lobby.returncode != 0:
            problems.append(f"lobby: failed, exit {lobby.returncode}")
        # The mods checked as a modder would check them
        checked = run([exe, "--check-mods", "--json"], env)
        mods, unread = check_mods_report(checked.stdout)
        problems += unread or check_mods_problems(mods)
        print(f"check-mods: exit {checked.returncode}, {len(mods)} mods checked")
        if checked.returncode != 1:
            problems.append(f"check-mods: exit {checked.returncode}, not 1 (errors)")
        named = run([exe, "--check-mods", str(documents / MODS_DIR / "OscModsFlowBroken")], env)
        print(named.stdout, end="")
        for want in BROKEN_FINDINGS:
            line = f"{want.file}:{want.line}: error:" if want.line else f"{want.file}: error:"
            if line not in named.stdout:
                problems.append(f"check-mods by folder: no {line!r} line")
        if "Checked 1 mod: 3 errors, 0 warnings." not in named.stdout:
            problems.append("check-mods by folder: not the broken mod's 3 errors alone")
        listed = run([exe, "--list-mods", "--json"], env)
        try:
            entries = cast(list[dict[str, object]], json.loads(listed.stdout))
            locations = {str(m.get("location")) for m in entries}
        except (json.JSONDecodeError, AttributeError, TypeError):
            locations = set[str]()
        for location in ("/mods/oscmodsflowgame", "/mods/oscmodsflowui", BROKEN_MOD):
            if location not in locations:
                problems.append(f"list-mods: {location} not listed (exit {listed.returncode})")
    for problem in problems:
        print(f"FAIL: {problem}")
    if problems:
        return 1
    print("mods flow: the sim and the game's UI have their mods")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
