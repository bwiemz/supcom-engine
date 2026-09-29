"""A game a matchmaking client launches over GPGNet, as FAF's does (M220b).

CTest: data.gpgnet_game. This script is the client for two games at once: a
host and a joiner, each started with `/gpgnet` (offscreen,
`--gpgnet-scripted`), `/players 2` and a `/team`, as FAF's client starts
them. Over GPGNet it drives retail's auto-lobby (autolobby.lua) as FAF's
matchmaker does:

- each says it is Idle; CreateLobby (init mode 1, the auto-lobby) with the
  player's uid, and each is in the Lobby state (each started with its
  faction, `/uef` or `/cybran`, as the matchmaker starts them);
- the host hosts SCMP_009; the joiner joins it, naming the host; each is
  told to connect to the other (ConnectToPeer);
- the host's auto-lobby sees both players established and launches, and
  both say they are Launching;
- once each has played PLAY_TICKS (their logs say), the client closes both
  links: each game ends, and must have played in lockstep with no desync
  or script error (its exit code), each reporting the tick it reached.

Usage:
    gpgnet_game.py <opensupcom> <port>

Ports: the host's link <port>, the joiner's <port>+1, their lobbies <port>+2
and <port>+3. Exits 77 (skipped) when the game has no data.
"""

from __future__ import annotations

import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from gpgnet_lobby import Link, listening

SKIPPED = 77
CONNECT_SECONDS = 120  # the games load FA's data first
REPLY_SECONDS = 30
LAUNCH_SECONDS = 120  # the launch loads the map
PLAY_SECONDS = 180  # loading the map, then the ticks
PLAY_TICKS = 100
EXIT_SECONDS = 60
TICK_LINE = re.compile(r"\[gpgnet\] tick (\d+)")


def ticks_logged(log: Path) -> int:
    """The latest tick a game's log says it reached."""
    found = TICK_LINE.findall(log.read_text(errors="replace")) if log.exists() else []
    return int(found[-1]) if found else 0


HOST_UID, JOINER_UID = 1234, 5678


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe, port = argv[0], int(argv[1])
    with tempfile.TemporaryDirectory(prefix="osc-gpgnet-game-") as tmp:
        return run(exe, port, Path(tmp))


def run(exe: str, port: int, root: Path) -> int:
    # Each role's link port, lobby port, team and faction (FAF's matchmaker
    # passes the faction: the auto-lobby leaves a random one unresolved)
    roles = {
        "host": (port, port + 2, "1", "/uef"),
        "joiner": (port + 1, port + 3, "2", "/cybran"),
    }
    servers: dict[str, socket.socket] = {}
    games: dict[str, subprocess.Popen[bytes]] = {}
    logs: dict[str, Path] = {}
    links: dict[str, Link] = {}
    try:
        for name, (link_port, _, team, faction) in roles.items():
            server = socket.socket()
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", link_port))
            server.listen(1)
            server.settimeout(CONNECT_SECONDS)
            servers[name] = server
            user_dir = root / name
            user_dir.mkdir()
            logs[name] = root / f"{name}.log"
            with logs[name].open("wb") as out:
                games[name] = subprocess.Popen(
                    [
                        exe,
                        "--user-dir",
                        str(user_dir),
                        "/gpgnet",
                        f"127.0.0.1:{link_port}",
                        "/players",
                        "2",
                        "/team",
                        team,
                        faction,
                        "--gpgnet-scripted",
                    ],
                    stdout=out,
                    stderr=subprocess.STDOUT,
                )
        for name, server in servers.items():
            try:
                conn, _ = server.accept()
            except TimeoutError:
                if games[name].poll() == SKIPPED:
                    return SKIPPED
                print(f"client: the {name} never connected")
                return 1
            links[name] = Link(conn)
            links[name].expect("GameState", ["Idle"], REPLY_SECONDS)

        host, joiner = links["host"], links["joiner"]
        host_lobby, joiner_lobby = roles["host"][1], roles["joiner"][1]
        host.send("CreateLobby", 1, host_lobby, "Host", HOST_UID, 0)
        host.expect("GameState", ["Lobby"], REPLY_SECONDS)
        joiner.send("CreateLobby", 1, joiner_lobby, "Joiner", JOINER_UID, 0)
        joiner.expect("GameState", ["Lobby"], REPLY_SECONDS)
        host.send("HostGame", "SCMP_009")
        deadline = time.monotonic() + REPLY_SECONDS
        while not listening(host_lobby):
            if time.monotonic() > deadline:
                print("client: the host's lobby never listened")
                return 1
            time.sleep(0.2)
        joiner.send("JoinGame", f"127.0.0.1:{host_lobby}", "Host", HOST_UID)
        # As FAF's client does, each is told of the other
        host.send("ConnectToPeer", f"127.0.0.1:{joiner_lobby}", "Joiner", JOINER_UID)
        joiner.send("ConnectToPeer", f"127.0.0.1:{host_lobby}", "Host", HOST_UID)
        host.expect("GameState", ["Launching"], LAUNCH_SECONDS)
        joiner.expect("GameState", ["Launching"], LAUNCH_SECONDS)
        print(f"client: both launched; playing to tick {PLAY_TICKS}")
        deadline = time.monotonic() + PLAY_SECONDS
        while min(ticks_logged(log) for log in logs.values()) < PLAY_TICKS:
            if time.monotonic() > deadline or any(g.poll() is not None for g in games.values()):
                print(
                    "client: the games didn't play on: "
                    + ", ".join(f"{name} at tick {ticks_logged(log)}" for name, log in logs.items())
                )
                return 1
            time.sleep(0.5)
        for link in links.values():
            link.conn.close()
        codes = {name: game.wait(timeout=EXIT_SECONDS) for name, game in games.items()}
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as err:
        print(f"client: {err}")
        return 1
    finally:
        for game in games.values():
            if game.poll() is None:
                game.kill()
                game.wait()
        for server in servers.values():
            server.close()
        for name, log in logs.items():
            if log.exists():
                lines = log.read_text(errors="replace").splitlines()
                print(f"--- the {name}'s log, its last lines ---")
                print("\n".join(lines[-15:]))

    failed = False
    for name, code in codes.items():
        text = logs[name].read_text(errors="replace")
        ticks = [int(t) for t in re.findall(r"\[gpgnet\] the game reached tick (\d+)", text)]
        print(f"client: the {name} ended (exit {code}), at tick {ticks[-1] if ticks else '-'}")
        if code != 0 or not ticks or ticks[-1] < PLAY_TICKS or "in lockstep" not in text:
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
