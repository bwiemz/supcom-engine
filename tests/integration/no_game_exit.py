#!/usr/bin/env python3
"""A run that finds no game exits 1, and 77 (skipped) under the gate's OSC_RETAIL_ONLY=1.

usage: no_game_exit.py <opensupcom>
"""

import os
import subprocess
import sys
import tempfile


def exit_code(exe: str, retail_only: bool) -> int:
    env = {k: v for k, v in os.environ.items() if not k.startswith("OSC_")}
    if retail_only:
        env["OSC_RETAIL_ONLY"] = "1"
    with tempfile.TemporaryDirectory() as user_dir:
        args = [exe, "--user-dir", user_dir, "--fa-path", os.path.join(user_dir, "none"), "--ticks", "1"]
        return subprocess.run(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode


def main() -> int:
    plain, gate = exit_code(sys.argv[1], False), exit_code(sys.argv[1], True)
    print(f"no game: exit {plain} plain (want 1), {gate} under OSC_RETAIL_ONLY=1 (want 77)")
    return 0 if (plain, gate) == (1, 77) else 1


if __name__ == "__main__":
    sys.exit(main())
