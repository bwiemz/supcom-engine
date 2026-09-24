"""The same replay, played by a Windows build and a Linux build, must match.

The cross-OS lockstep check (roadmap M199c). A replay is played by this
machine's Linux build and by a Windows build under Wine, both on the same
Forged Alliance data, each writing `--checksum-trace`; the traces are compared
with tools/checksum_diff.py, which names the first tick and part that differ.
Each playback also checks itself against the checksums the replay recorded.

The Windows build is CI's (the `opensupcom-windows` artifact of a workflow
run, fetched with `gh`) or a local directory holding opensupcom.exe,
osc_integration.exe and their DLLs. The Windows side runs the program the
Linux side does: opensupcom, or the integration runner. The replay is a
file, or is recorded first with the Linux build from game arguments given
after `--`.

With --direct, nothing is recorded: both builds run the game arguments as
they are, each writing its trace. That suits scripted scenarios such as the
integration tests (`--weapon-test`, `--aim-test`; give the runner,
osc_integration, as --linux-exe), which fight from the first tick, where a
recorded AI game may never see a shot.

Examples:
    cross_os_replay.py --run-id 123456789 --linux-exe build/linux-debug/opensupcom \\
        -- --map /maps/SCMP_009/SCMP_009_scenario.lua --ai-skirmish --ai-armies 4 \\
           --ticks 1200 --seed 4242 --scripted-orders
    cross_os_replay.py --windows-dir win/ --linux-exe build/linux-debug/opensupcom \\
        --replay game.oscreplay
    cross_os_replay.py --run-id 123456789 --linux-exe build/linux-release/osc_integration \\
        --direct -- --map /maps/SCMP_009/SCMP_009_scenario.lua --aim-test

Needs Forged Alliance (--fa-path, or OSC_FA_PATH, or the Steam install the
engine finds by itself) and Wine. It isn't a CI test: CI has no game data.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ARTIFACT = "opensupcom-windows"
TIMEOUT_SECONDS = 1800


def windows_path(path: Path, env: dict[str, str]) -> str:
    """`path` as the Windows build sees it under Wine (Z:\\...)."""
    out = subprocess.run(
        ["winepath", "-w", str(path)], env=env, capture_output=True, text=True, check=False
    )
    if out.returncode != 0 or not out.stdout.strip():
        sys.exit(f"winepath failed for {path} (exit {out.returncode}):\n{out.stderr[-2000:]}")
    return out.stdout.strip()


def fetch_windows_build(run_id: str, repo: str | None, into: Path) -> Path:
    cmd = ["gh", "run", "download", run_id, "-n", ARTIFACT, "-D", str(into)]
    if repo:
        cmd += ["-R", repo]
    _ = subprocess.run(cmd, check=True)
    return into


def find_fa_path(explicit: str | None, linux_exe: Path) -> Path:
    if explicit:
        return Path(explicit)
    if env_path := os.environ.get("OSC_FA_PATH"):
        return Path(env_path)
    # The engine's own install search: `--print-install` prints fa_path=<dir>.
    out = subprocess.run(
        [str(linux_exe), "--print-install"], capture_output=True, text=True, check=False
    )
    for line in out.stdout.splitlines():
        if line.startswith("fa_path="):
            return Path(line.removeprefix("fa_path="))
    sys.exit("Forged Alliance not found: pass --fa-path")


def play(cmd: list[str], env: dict[str, str] | None, what: str) -> int:
    print(f"== {what}")
    done = subprocess.run(
        cmd, env=env, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False
    )
    for line in done.stdout.splitlines():
        if line.startswith("REPLAY"):
            print(f"   {line}")
    if done.returncode != 0:
        print(f"   exit {done.returncode}")
        print("\n".join(done.stderr.splitlines()[-20:]))
    return done.returncode


DEFAULT_WINE_PREFIX = str(Path.home() / ".local/share/osc-wine")


class Args(argparse.Namespace):
    # The defaults live here, not in add_argument: argparse leaves an
    # attribute the namespace already has alone.
    run_id: str | None = None
    windows_dir: str | None = None
    repo: str | None = None
    linux_exe: str = ""
    replay: str | None = None
    fa_path: str | None = None
    wine_prefix: str = DEFAULT_WINE_PREFIX
    direct: bool = False


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    source = parser.add_mutually_exclusive_group(required=True)
    _ = source.add_argument("--run-id", help="CI workflow run whose Windows build to use")
    _ = source.add_argument(
        "--windows-dir", help="directory with opensupcom.exe, osc_integration.exe and their DLLs"
    )
    _ = parser.add_argument("--repo", help="GitHub repository of --run-id (default: this one)")
    _ = parser.add_argument(
        "--linux-exe", required=True, help="the Linux opensupcom (or osc_integration)"
    )
    _ = parser.add_argument("--replay", help="replay to play (else one is recorded first)")
    _ = parser.add_argument(
        "--direct",
        action="store_true",
        help="run the game arguments on both builds instead of recording and replaying",
    )
    _ = parser.add_argument("--fa-path", help="Forged Alliance install (default: found)")
    _ = parser.add_argument(
        "--wine-prefix", help=f"Wine prefix to run the Windows build in ({DEFAULT_WINE_PREFIX})"
    )
    args, game_args = parser.parse_known_args(namespace=Args())
    if game_args and game_args[0] == "--":
        game_args = game_args[1:]
    if not args.replay and not game_args:
        parser.error("give --replay, or game arguments after -- to record one")
    if args.direct and (args.replay or not game_args):
        parser.error("--direct runs game arguments given after --, not a replay")

    for tool in ("wine", "winepath"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")
    linux_exe = Path(args.linux_exe).resolve()
    fa_path = find_fa_path(args.fa_path, linux_exe)
    env = dict(os.environ)
    env.update(
        WINEPREFIX=args.wine_prefix,
        WINEDEBUG="-all",
        # No Mono/Gecko install prompts: the engine needs neither.
        WINEDLLOVERRIDES="mscoree=;mshtml=",
    )
    # Settle the prefix first: the first command in a new (or newer-Wine)
    # prefix updates it, and a winepath run during the update fails.
    _ = subprocess.run(["wineboot"], env=env, capture_output=True, check=False)

    with tempfile.TemporaryDirectory(prefix="osc-cross-os-") as tmp_dir:
        tmp = Path(tmp_dir)
        if args.run_id:
            win_dir = fetch_windows_build(args.run_id, args.repo, tmp / "win")
        else:
            win_dir = Path(args.windows_dir or ".").resolve()
        # The same program as the Linux side's: the game, or the runner.
        win_exe = win_dir / f"{linux_exe.stem}.exe"
        if not win_exe.exists():
            sys.exit(f"no {win_exe.name} in {win_dir}")

        replay = Path(args.replay).resolve() if args.replay else tmp / "game.oscreplay"
        # What each build runs: the replay, or with --direct the game itself.
        linux_run = ["--replay", str(replay)]
        windows_run = ["--replay", windows_path(replay, env)] if not args.direct else []
        if args.direct:
            linux_run = windows_run = game_args
        elif not args.replay:
            code = play(
                [str(linux_exe), "--fa-path", str(fa_path), *game_args, "--record", str(replay)],
                None,
                "record (Linux)",
            )
            if code != 0:
                return 1

        linux_trace, windows_trace = tmp / "linux.txt", tmp / "windows.txt"
        linux_code = play(
            [
                str(linux_exe),
                "--fa-path",
                str(fa_path),
                *linux_run,
                "--checksum-trace",
                str(linux_trace),
            ],
            None,
            "play (Linux)",
        )
        windows_code = play(
            [
                "wine",
                str(win_exe),
                "--fa-path",
                windows_path(fa_path, env),
                *windows_run,
                "--checksum-trace",
                windows_path(windows_trace, env),
            ],
            env,
            "play (Windows, under Wine)",
        )
        if not linux_trace.exists() or not windows_trace.exists():
            print("a playback wrote no trace")
            return 1
        diff = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("checksum_diff.py")),
                str(linux_trace),
                str(windows_trace),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        print(diff.stdout, end="")
        return 0 if diff.returncode == 0 and linux_code == 0 and windows_code == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
