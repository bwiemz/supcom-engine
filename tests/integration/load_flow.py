"""A saved game loads in the game through retail's Load dialog path.

CTest: data.load_flow. Plays a short game headless and saves it (`--save`,
`--save-at`) into a temporary user folder, as `savegames/Player/flow.oscsave`,
then runs `--user-dir <folder> --load-flow-test`. The engine lists the folder
with GetSpecialFiles, refuses a missing save as 'CantOpen', and loads the
listed one with LoadSavedGame, as retail's Load dialog does. Offscreen, the
game is restored from the save's snapshot at the saved tick (checked against
the save; the UI never takes it for a replay), plays on, and is saved again
with InternalSaveGame;
the new save must hold the whole game, and a save outside the folder is
refused. Then, as a player might go on: the new save loads from inside the
game, as the game menu's Load dialog loads it; the game returns to the lobby
(ReturnToLobby); and the first save loads from the front end again. Each
game starts in a fresh UI Lua state, and no Lua error may happen.

Then the same flow runs on a copy of the save whose snapshot is damaged
past its outer check (its Lua heap's own hash fails, after the sim's C++
state was already replaced): each load must fall back to booting the game
again and catching up from the history, and pass all the same.

Usage:
    load_flow.py <opensupcom> -- <game args for the first game...>

The game args must run past SAVE_AT. Exits 77 (skipped) when the game has no
data to run on.
"""

from __future__ import annotations

import hashlib
import hmac
import subprocess
import sys
import tempfile
import zlib
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
            [
                str(exe),
                *game_args,
                "--user-dir",
                str(user_dir),
                "--save",
                str(save),
                "--save-at",
                str(SAVE_AT),
            ],
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

        if run_flow(exe, user_dir, must_say="restored at tick") != 0:
            return 1

        # The fallback: a damaged snapshot, each load catching up instead
        damaged_dir = Path(tmp) / "damaged"
        damaged = damaged_dir / "savegames" / "Player" / "flow.oscsave"
        damaged.parent.mkdir(parents=True)
        key = (user_dir / KEY_FILE).read_bytes()
        _ = (damaged_dir / KEY_FILE).write_bytes(key)
        _ = damaged.write_bytes(damage_snapshot(save.read_bytes(), key))
        return run_flow(exe, damaged_dir, must_say="not restored")


KEY_FILE = "opensupcom-snapshot.key"


def snapshot_hash(b: bytes) -> int:
    """src/sim/sim_snapshot.cpp's snapshot_hash."""
    mask = (1 << 64) - 1
    h = 0xCBF29CE484222325
    i = 0
    while i + 8 <= len(b):
        h = ((h ^ int.from_bytes(b[i : i + 8], "little")) * 0x9E3779B97F4A7C15) & mask
        h ^= h >> 32
        i += 8
    for c in b[i:]:
        h = ((h ^ c) * 0x100000001B3) & mask
    return h


def damage_snapshot(save: bytes, key: bytes) -> bytes:
    """The save with a byte of its snapshot's Lua heap flipped, the
    snapshot's own hash and its signature made good again
    (SavedGame::serialize's layout)."""
    pos = 7 + 4  # magic, version
    for _ in range(2):  # build, name
        pos += 4 + int.from_bytes(save[pos : pos + 4], "little")
    pos += 4  # tick
    raw_size = int.from_bytes(save[pos : pos + 8], "little")
    packed_size = int.from_bytes(save[pos + 8 : pos + 16], "little")
    pos += 16
    snap = bytearray(zlib.decompress(save[pos : pos + packed_size]))
    assert len(snap) == raw_size
    snap[len(snap) - 64] ^= 0x40  # in the Lua heap, which comes last
    snap[-8:] = snapshot_hash(bytes(snap[:-8])).to_bytes(8, "little")
    packed = zlib.compress(bytes(snap), 1)
    mac = hmac.new(key, bytes(snap), hashlib.sha256).digest()
    head = bytearray(save[: pos - 16])
    head += len(snap).to_bytes(8, "little") + len(packed).to_bytes(8, "little")
    return bytes(head) + packed + mac + save[pos + packed_size + len(mac) :]


def run_flow(exe: Path, user_dir: Path, must_say: str = "") -> int:
    flow = subprocess.run(
        [str(exe), "--user-dir", str(user_dir), "--load-flow-test"],
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
        check=False,
    )
    for line in flow.stdout.splitlines():
        if "load-flow" in line or line.startswith("LOAD") or "not restored" in line:
            print(line)
    if flow.returncode == 0 and must_say and must_say not in flow.stdout:
        print(f"the flow never said '{must_say}'")
        return 1
    return flow.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
