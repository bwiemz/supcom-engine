#!/usr/bin/env python3
"""A custom game as FAF's client hosts one, on FAF's data: the "FAF client"
step of a validation record (docs/validation/README.md).

FAF's client starts the game with `/gpgnet`, makes a lobby and hosts a map
over GPGNet; the player then adds an AI and launches. This plays both parts:

- It makes a scratch copy of a FAF install (`init_faf.lua` and `fa_path.lua`
  copied, `gamedata` linked) whose init mounts one directory ahead of the
  rest. In it is FAF's own `lobby.lua` with a driver appended: once the game
  is hosting, it adds an AI to slot 2, readies the host and launches.
- It stands in for the client over GPGNet: Idle, CreateLobby, Lobby,
  HostGame, Launching (tests/integration/gpgnet_lobby.py's Link).
- It waits for the game's log to show the tick asked for (`[gpgnet] tick N`,
  every 50), closes the link, and takes the game's exit code: nonzero on any
  script error or desync.

Only `init_faf.lua` and `fa_path.lua` are read from the install's own files;
nothing else in it is opened, and the game writes to a scratch user folder.

Usage:
    faf_custom_game.py <opensupcom> [--faf <install>] [--port <p>] [--ticks <n>]
                       [--map <name>] [--log <file>]

Defaults: the install ~/.faforever, port 47100 (the lobby takes the next),
3000 ticks, SCMP_009. Exits 0 when the game played to the tick and ended
cleanly, 1 when it didn't, 2 on bad usage, 77 when there is no FAF install.
"""

from __future__ import annotations

import importlib.util
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile
from collections.abc import Callable
from pathlib import Path
from typing import Protocol, cast

SKIPPED = 77
CONNECT_SECONDS = 180  # the game loads FA's and FAF's data first
REPLY_SECONDS = 60
EXIT_SECONDS = 120
TICK_SECONDS_PER_THOUSAND = 120  # slack for a slow machine

# Appended to FAF's lobby.lua, where it sees the file's locals. HostUtils is
# made once the game is hosting (InitHostUtils).
DRIVER = """

-- faf_custom_game.py: play the host, as a player would.
ForkThread(function()
    while not HostUtils do WaitSeconds(0.5) end
    WaitSeconds(1)
    HostUtils.AddAI('AI: Easy', 'easy', 2)
    WaitSeconds(1)
    SetPlayerOption(1, 'Ready', true)
    WaitSeconds(1)
    TryLaunch(false)
end)
"""

# Inserted into init_faf.lua after its mount table starts, so the scratch
# directory is mounted first and its lobby.lua is the one found.
MOUNT_MARK = "local UpvaluedPathNext = 1\n"


class Link(Protocol):
    """The client's end of GPGNet, as gpgnet_lobby.py's Link has it."""

    def send(self, name: str, *args: int | str) -> None: ...

    def expect(self, name: str, args: list[int | str], seconds: float) -> None: ...


def load_link() -> Callable[[socket.socket], Link]:
    """tests/integration/gpgnet_lobby.py's Link: the client's end of GPGNet."""
    path = Path(__file__).resolve().parent.parent / "tests" / "integration" / "gpgnet_lobby.py"
    spec = importlib.util.spec_from_file_location("gpgnet_lobby", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"can't load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return cast(Callable[[socket.socket], Link], module.Link)


def option(args: list[str], name: str, default: str) -> str:
    return args[args.index(name) + 1] if name in args else default


def scratch_install(faf: Path, root: Path) -> Path:
    """The scratch copy of `faf`, mounting `root/mount` first. Its path."""
    install = root / "faf"
    (install / "bin").mkdir(parents=True)
    init = (faf / "bin" / "init_faf.lua").read_text(encoding="utf-8", errors="replace")
    if MOUNT_MARK not in init:
        raise RuntimeError("init_faf.lua has no mount table to put the scratch mount in")
    mount = root / "mount"
    first = (
        f"UpvaluedPath[1] = {{ dir = '{mount.as_posix()}', mountpoint = '/' }}\n"
        "UpvaluedPathNext = 2\n"
    )
    _ = (install / "bin" / "init_faf.lua").write_text(
        init.replace(MOUNT_MARK, MOUNT_MARK + first, 1), encoding="utf-8"
    )
    _ = shutil.copy(faf / "fa_path.lua", install / "fa_path.lua")
    (install / "gamedata").symlink_to(faf / "gamedata", target_is_directory=True)

    with zipfile.ZipFile(faf / "gamedata" / "lua.nx2") as lua:
        lobby = lua.read("lua/ui/lobby/lobby.lua").decode("utf-8", errors="replace")
    target = mount / "lua" / "ui" / "lobby" / "lobby.lua"
    target.parent.mkdir(parents=True)
    _ = target.write_text(lobby + DRIVER, encoding="utf-8")
    return install


def log_has(log: Path, text: str) -> bool:
    return log.exists() and text in log.read_text(encoding="utf-8", errors="replace")


def play(
    exe: str, install: Path, user: Path, port: int, ticks: int, map_name: str, log: Path
) -> int:
    link_class = load_link()
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(1)
    server.settimeout(CONNECT_SECONDS)
    with log.open("w", encoding="utf-8") as out:
        game = subprocess.Popen(
            [
                exe,
                "--user-dir",
                str(user),
                "--faf-data",
                str(install),
                "/gpgnet",
                f"127.0.0.1:{port}",
                "--gpgnet-scripted",
            ],
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        try:
            try:
                conn = server.accept()[0]
            except TimeoutError:
                code = game.poll()
                print(f"client: the game never connected (exit {code})")
                return SKIPPED if code == SKIPPED else 1
            link = link_class(conn)
            link.expect("GameState", ["Idle"], REPLY_SECONDS)
            link.send("CreateLobby", 0, port + 1, "Tester", 42, 1)
            link.expect("GameState", ["Lobby"], REPLY_SECONDS)
            link.send("HostGame", map_name)
            link.expect("GameState", ["Launching"], REPLY_SECONDS)
            print("client: the game launched")
            marker = f"[gpgnet] tick {ticks}\n"
            deadline = time.monotonic() + TICK_SECONDS_PER_THOUSAND * max(1, ticks // 1000)
            while not log_has(log, marker):
                if game.poll() is not None:
                    print(f"client: the game ended before tick {ticks} (exit {game.returncode})")
                    return 1
                if time.monotonic() > deadline:
                    print(f"client: no tick {ticks} in time")
                    return 1
                time.sleep(1.0)
            print(f"client: the game reached tick {ticks}")
            conn.close()
            code = game.wait(timeout=EXIT_SECONDS)
            print(f"client: the game ended (exit {code})")
            return code
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as err:
            print(f"client: {err}")
            return 1
        finally:
            if game.poll() is None:
                game.kill()
                _ = game.wait()
            server.close()


def main(argv: list[str]) -> int:
    if not argv or argv[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    exe = argv[0]
    faf = Path(option(argv, "--faf", str(Path.home() / ".faforever"))).expanduser()
    port = int(option(argv, "--port", "47100"))
    ticks = int(option(argv, "--ticks", "3000"))
    map_name = option(argv, "--map", "SCMP_009")
    if ticks % 50 != 0:
        print("--ticks must be a multiple of 50 (the game logs every 50)", file=sys.stderr)
        return 2
    if not (faf / "bin" / "init_faf.lua").exists() or not (faf / "gamedata" / "lua.nx2").exists():
        print(f"no FAF install at {faf}: skipped")
        return SKIPPED
    with tempfile.TemporaryDirectory(prefix="osc-faf-game-") as tmp:
        root = Path(tmp)
        install = scratch_install(faf, root)
        (root / "user").mkdir()
        log = Path(option(argv, "--log", str(root / "game.log")))
        code = play(exe, install, root / "user", port, ticks, map_name, log)
        if code not in (0, SKIPPED):
            lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
            print("--- the game's log, its last lines ---")
            print("\n".join(lines[-30:]))
        return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
