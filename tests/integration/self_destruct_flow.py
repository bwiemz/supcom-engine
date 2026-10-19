"""A windowed game whose player is ARMY_2 replays headless.

CTest: data.self_destruct_flow. Runs `--self-destruct-flow-test` in a
temporary user folder: a skirmish launched from the front end with an AI in
the first slot and the player in the second, played offscreen; the player
self-destructs their commander as Ctrl-K does. The game's UI focuses ARMY_2;
its recording (LastGame) then plays headless with `--replay`, where nothing
focuses it, and must match the recording at every beat. Whatever the UI alone
decides must not reach the sim's checksum.

The two runs' checksum traces must then agree in every compared domain to the
game's last tick, and differ in the diagnostic `threads` column: the
commander's death forks a script thread for the focus army alone, so the
scenario reaches the input it guards against.

Usage:
    self_destruct_flow.py <opensupcom> <checksum_diff.py>

Exits 77 (skipped) when the game has no data.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 900
SKIPPED = 77


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args, capture_output=True, text=True, timeout=TIMEOUT_SECONDS, check=False
    )


def load_checksum_diff(path: str):
    spec = importlib.util.spec_from_file_location("checksum_diff", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["checksum_diff"] = module  # dataclasses look their module up
    spec.loader.exec_module(module)
    return module


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe, diff_tool = argv
    with tempfile.TemporaryDirectory(prefix="osc-self-destruct-flow-") as tmp:
        user_dir = Path(tmp)
        live_trace, replay_trace = user_dir / "live.txt", user_dir / "replay.txt"
        game = run(
            [
                exe,
                "--user-dir",
                str(user_dir),
                "--self-destruct-flow-test",
                "--checksum-trace",
                str(live_trace),
            ]
        )
        if game.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        for line in game.stdout.splitlines():
            if "self-destruct-flow" in line:
                print(line)
        replay_file = user_dir / "replays" / "Player" / "LastGame.oscreplay"
        if game.returncode != 0 or not replay_file.exists():
            print(
                f"the game failed: exit {game.returncode}, {replay_file} exists: "
                f"{replay_file.exists()}"
            )
            return 1

        replay = run([exe, "--replay", str(replay_file), "--checksum-trace", str(replay_trace)])
        for line in replay.stdout.splitlines():
            if line.startswith("REPLAY"):
                print(line)
        tool = load_checksum_diff(diff_tool)
        live, played = tool.read_trace(live_trace), tool.read_trace(replay_trace)
        if replay.returncode != 0:
            diff = run([sys.executable, diff_tool, str(live_trace), str(replay_trace)])
            print(diff.stdout, end="")
            return 1
        failures = []
        if not played.ticks or played.ticks[-1].tick != live.ticks[-1].tick:
            failures.append(
                f"the replay's trace ends at tick "
                f"{played.ticks[-1].tick if played.ticks else 0}, the game's at "
                f"{live.ticks[-1].tick}"
            )
        compared = tool.first_difference(live, played, diagnostic=False)
        if compared:
            failures.append(f"the compared domains differ at tick {compared.tick}")
        threads = tool.first_difference(live, played, diagnostic=True)
        if threads:
            print(f"threads first differ at tick {threads.tick} (focus-gated fork reached)")
        else:
            failures.append("the threads never differ: the focus-gated fork wasn't reached")
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
