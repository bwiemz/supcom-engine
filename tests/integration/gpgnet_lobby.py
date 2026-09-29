"""The game, launched by a matchmaking client over GPGNet (M220a).

CTest: data.gpgnet_lobby. This script is the client (FAF's, or its ICE
adapter's, part): it listens, starts the game with `/gpgnet 127.0.0.1:<port>`
(offscreen, `--gpgnet-scripted`), and speaks GPGNet to it:

- the game connects and says it is Idle;
- CreateLobby makes a lobby through retail's onlineprovider.lua, and the
  game says it is in the Lobby state;
- HostGame hosts it on the lobby's port, which then takes a connection;
- the client closes the link, and the game, with nothing to do, ends.

Any script error fails it (the game's exit code).

Usage:
    gpgnet_lobby.py <opensupcom> <port>

The lobby's port is <port> + 1. Exits 77 (skipped) when the game has no data.
"""

from __future__ import annotations

import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SKIPPED = 77
CONNECT_SECONDS = 120  # the game loads FA's data first
REPLY_SECONDS = 30
EXIT_SECONDS = 60


def encode(name: str, *args: int | str) -> bytes:
    """A GPGNet command: the name, then each argument's type and body."""
    out = struct.pack("<I", len(name)) + name.encode()
    out += struct.pack("<I", len(args))
    for arg in args:
        if isinstance(arg, int):
            out += b"\x00" + struct.pack("<i", arg)
        else:
            data = arg.encode()
            out += b"\x01" + struct.pack("<I", len(data)) + data
    return out


class Link:
    """The client's end of the link: commands out, whole commands in."""

    def __init__(self, conn: socket.socket) -> None:
        self.conn = conn
        self.buf = b""

    def send(self, name: str, *args: int | str) -> None:
        self.conn.sendall(encode(name, *args))

    def _take(self) -> tuple[str, list[int | str]] | None:
        """The command at the front of the buffer, if it is whole."""
        at = 0

        def need(n: int) -> bool:
            return len(self.buf) - at >= n

        if not need(4):
            return None
        (size,) = struct.unpack_from("<I", self.buf, at)
        at += 4
        if not need(size + 4):
            return None
        name = self.buf[at : at + size].decode()
        at += size
        (argc,) = struct.unpack_from("<I", self.buf, at)
        at += 4
        args: list[int | str] = []
        for _ in range(argc):
            if not need(1):
                return None
            kind = self.buf[at]
            at += 1
            if kind == 0:
                if not need(4):
                    return None
                args.append(struct.unpack_from("<i", self.buf, at)[0])
                at += 4
            else:
                if not need(4):
                    return None
                (length,) = struct.unpack_from("<I", self.buf, at)
                at += 4
                if not need(length):
                    return None
                args.append(self.buf[at : at + length].decode(errors="replace"))
                at += length
        self.buf = self.buf[at:]
        return name, args

    def expect(self, name: str, args: list[int | str], seconds: float) -> None:
        """Wait for the game to send `name` with `args` (others are noted)."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            while (command := self._take()) is not None:
                print(f"client: heard {command[0]} {command[1]}")
                if command == (name, args):
                    return
            self.conn.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                data = self.conn.recv(4096)
            except TimeoutError:
                continue
            if not data:
                raise RuntimeError(f"the game closed the link, waiting for {name} {args}")
            self.buf += data
        raise RuntimeError(f"no {name} {args} from the game in {seconds} s")


def listening(port: int) -> bool:
    with socket.socket() as probe:
        probe.settimeout(1)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def run(exe: str, port: int, user_dir: Path) -> int:
    lobby_port = port + 1
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(1)
    server.settimeout(CONNECT_SECONDS)
    game = subprocess.Popen(
        [exe, "--user-dir", str(user_dir), "/gpgnet", f"127.0.0.1:{port}", "--gpgnet-scripted"]
    )
    try:
        try:
            conn, _ = server.accept()
        except TimeoutError:
            code = game.poll()
            if code == SKIPPED:
                return SKIPPED
            print(f"client: the game never connected (exit {code})")
            return 1
        link = Link(conn)
        link.expect("GameState", ["Idle"], REPLY_SECONDS)
        link.send("CreateLobby", 0, lobby_port, "Tester", 42, 1)
        link.expect("GameState", ["Lobby"], REPLY_SECONDS)
        link.send("HostGame", "SCMP_009")
        deadline = time.monotonic() + REPLY_SECONDS
        while not listening(lobby_port):
            if time.monotonic() > deadline:
                print(f"client: the game's lobby never listened on {lobby_port}")
                return 1
            time.sleep(0.2)
        print(f"client: the game's lobby listens on {lobby_port}")
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
            game.wait()
        server.close()


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="osc-gpgnet-") as tmp:
        return run(argv[0], int(argv[1]), Path(tmp))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
