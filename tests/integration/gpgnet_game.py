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

With --relay-loss <fraction> the two games reach each other as they do
through FAF's ICE adapter (M220c): each through a local UDP port standing
for the other, which relays what comes, losing that fraction of it.

Usage:
    gpgnet_game.py <opensupcom> <port> [--relay-loss <fraction>]

Ports: the host's link <port>, the joiner's <port>+1, their lobbies <port>+2
and <port>+3, the relay's <port>+4 (the host, as the joiner reaches it) and
<port>+5 (the joiner, as the host sees it). Exits 77 (skipped) when the game
has no data.
"""

from __future__ import annotations

import random
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
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


class Relay:
    """FAF's ICE adapter, as the games see it: one local UDP port standing
    for each player at the other's side. What the joiner sends to the host's
    stand-in goes to the host from the joiner's stand-in, and the other way
    round, a fraction of it lost."""

    def __init__(
        self, host_side: int, joiner_side: int, host_lobby: int, joiner_lobby: int, loss: float
    ) -> None:
        self.for_host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.for_host.bind(("127.0.0.1", host_side))
        self.for_joiner = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.for_joiner.bind(("127.0.0.1", joiner_side))
        self.host_lobby, self.joiner_lobby = host_lobby, joiner_lobby
        self.loss = loss
        self.rng = random.Random(2026)
        self.stop = False
        self.carried = self.lost = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self) -> None:
        while not self.stop:
            ready, _, _ = select.select([self.for_host, self.for_joiner], [], [], 0.05)
            for sock in ready:
                data, _ = sock.recvfrom(4096)
                if self.rng.random() < self.loss:
                    self.lost += 1
                    continue
                self.carried += 1
                if sock is self.for_host:  # the joiner's, to the host
                    self.for_joiner.sendto(data, ("127.0.0.1", self.host_lobby))
                else:  # the host's, to the joiner
                    self.for_host.sendto(data, ("127.0.0.1", self.joiner_lobby))

    def close(self) -> None:
        self.stop = True
        self.thread.join()
        self.for_host.close()
        self.for_joiner.close()


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    exe, port = argv[0], int(argv[1])
    loss = float(argv[argv.index("--relay-loss") + 1]) if "--relay-loss" in argv else None
    with tempfile.TemporaryDirectory(prefix="osc-gpgnet-game-") as tmp:
        return run(exe, port, Path(tmp), loss)


def run(exe: str, port: int, root: Path, loss: float | None) -> int:
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
    relay: Relay | None = None
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
        # Each reaches the other directly, or through the relay's stand-ins
        host_at, joiner_at = host_lobby, joiner_lobby
        if loss is not None:
            relay = Relay(port + 4, port + 5, host_lobby, joiner_lobby, loss)
            host_at, joiner_at = port + 4, port + 5
        joiner.send("JoinGame", f"127.0.0.1:{host_at}", "Host", HOST_UID)
        # As FAF's client does, each is told of the other
        host.send("ConnectToPeer", f"127.0.0.1:{joiner_at}", "Joiner", JOINER_UID)
        joiner.send("ConnectToPeer", f"127.0.0.1:{host_at}", "Host", HOST_UID)
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
        if relay is not None:
            relay.close()
            print(f"client: the relay carried {relay.carried} datagrams, lost {relay.lost}")
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
