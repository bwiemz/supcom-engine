#!/usr/bin/env python3
"""Install the build into a temporary prefix and run what was installed (M227a).

Checks that `cmake --install` lays out what a package carries -- the game,
its documents, and on Linux its desktop entry and icon -- and that the
installed game runs on its own: `opensupcom --version` prints this build's
version. (On Windows that also proves the DLLs it needs were installed
beside it.)

Usage: check_install.py <build dir> <config> <version>   (exit 1 on a problem)
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


def expected_files(windows: bool) -> list[str]:
    if windows:
        # (vcruntime140.dll: the Visual C++ runtime installed beside it)
        return ["opensupcom.exe", "README.md", "LICENSE", "CHANGELOG.md", "vcruntime140.dll"]
    return [
        "bin/opensupcom",
        "share/doc/opensupcom/README.md",
        "share/doc/opensupcom/LICENSE",
        "share/doc/opensupcom/CHANGELOG.md",
        "share/applications/opensupcom.desktop",
        "share/icons/hicolor/scalable/apps/opensupcom.svg",
    ]


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__)
        return 2
    build, config, version = Path(argv[1]), argv[2], argv[3]
    windows = sys.platform == "win32"
    with tempfile.TemporaryDirectory(prefix="osc-install-") as tmp:
        prefix = Path(tmp)
        cmd = ["cmake", "--install", str(build), "--prefix", str(prefix)]
        if config:
            cmd += ["--config", config]
        installed = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if installed.returncode != 0:
            print(installed.stdout, installed.stderr)
            print("cmake --install failed")
            return 1

        missing = [f for f in expected_files(windows) if not (prefix / f).is_file()]
        for f in missing:
            print(f"not installed: {f}")

        game = prefix / expected_files(windows)[0]
        if not game.is_file():
            return 1  # (reported as not installed)
        ran = subprocess.run(
            [str(game), "--version"], capture_output=True, text=True, check=False, timeout=60
        )
        line = ran.stdout.strip()
        # "OpenSupCom <version> (<revision>)", and nothing else
        ok = (
            ran.returncode == 0
            and re.fullmatch(rf"OpenSupCom {re.escape(version)} \(\S+\)", line) is not None
        )
        if not ok:
            print(
                f"installed --version: exit {ran.returncode}, output {ran.stdout!r} {ran.stderr!r}"
            )
        else:
            print(f"installed --version: {line}")
        return 0 if ok and not missing else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
