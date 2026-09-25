"""A saved game loads in the game through retail's Load dialog path.

CTest: data.load_flow. Plays a short game headless and saves it (`--save`,
`--save-at`) into a temporary user folder, as `savegames/Player/flow.oscsave`,
then runs `--user-dir <folder> --load-flow-test`. The engine lists the folder
with GetSpecialFiles, refuses a missing save as 'CantOpen', and loads the
listed one with LoadSavedGame, as retail's Load dialog does. Offscreen, the
game catches up to the saved tick (checked against the save; the UI never
takes it for a replay), plays on, and is saved again with InternalSaveGame;
the new save must hold the whole game, and a save outside the folder is
refused.

Usage:
    load_flow.py <opensupcom> -- <game args for the first game...>

The game args must run past SAVE_AT. Exits 77 (skipped) when the game has no
data to run on.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 900
SKIPPED = 77
SAVE_AT = 200


def main(argv: list[str]) -> int:
    if len(argv) < 2 or "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    exe = Path(argv[0])
    game_args = argv[argv.index("--") + 1 :]
    with tempfile.TemporaryDirectory(prefix="osc-load-flow-") as tmp:
        user_dir = Path(tmp)
        save = user_dir / "savegames" / "Player" / "flow.oscsave"
        game = subprocess.run(
            [str(exe), *game_args, "--save", str(save), "--save-at", str(SAVE_AT)],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        if game.returncode == SKIPPED:
            print("no game data: skipped")
            return SKIPPED
        if game.returncode != 0 or not save.exists():
            print(f"the game run failed or saved nothing: exit {game.returncode}")
            return 1

        flow = subprocess.run(
            [str(exe), "--user-dir", str(user_dir), "--load-flow-test"],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        for line in flow.stdout.splitlines():
            if "load-flow" in line or line.startswith("LOAD"):
                print(line)
        return flow.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
