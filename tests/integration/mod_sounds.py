"""A mod's sounds play with FA's: its sounds folder, mounted at /sounds.

CTest: data.mod_sounds. FAF's init file (init_faf.lua's MountModContent and
MountMapContent) mounts each mod's and map's `sounds` folder at /sounds,
ahead of FA's own, and Moho's audio engine lists its banks there through
the VFS. A mod's units (BlackOps', Total Mayhem's) play their own banks so.

A temporary mod holds a sound bank and its wave bank, written here (an XACT
3.0 sound bank with one cue, a wave bank of one 0.1 s wave). An init file
mounts retail's data as SupComDataPath.lua does, with the mod's sounds folder
at /sounds first. `--audio-data-test` then reports the banks the engine
loaded: the mod's must be among FA's, and every cue of every bank, the mod's
too, must resolve to a playable wave.

Usage:
    mod_sounds.py <osc_integration>

Exits 77 (skipped) when the game has no data.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 600
SKIPPED = 77

MOD_BANK = "OscModSounds"
MOD_WAVES = "OscModSoundsWaves"
MOD_CUE = "OscModClick"


def name64(name: str) -> bytes:
    return name.encode().ljust(64, b"\0")


def sound_bank(name: str, wave_bank: str, cue: str) -> bytes:
    """An XACT 3.0 sound bank `name` with one cue, `cue`: a simple sound
    (category 0, Global) playing wave 0 of wave bank `wave_bank` once."""
    head = b"SDBK" + struct.pack("<HHH", 43, 43, 0) + bytes(8) + b"\x01"
    head += struct.pack("<HHHH", 1, 0, 0, 1)  # simple, complex, unknown, total cues
    head += struct.pack("<BHHH", 1, 1, 0, 0)  # wave banks, sounds, name length, unknown
    offsets_at = len(head)
    data = bytearray(head + bytes(10 * 4) + name64(name))
    wave_bank_names = len(data)
    data += name64(wave_bank)
    sounds = len(data)
    # flags, category, volume (0 mB), pitch, priority, entry length, wave, wave bank
    data += struct.pack("<BHBhBHHB", 0x00, 0, 180, 0, 0, 12, 0, 0)
    simple_cues = len(data)
    data += struct.pack("<BI", 0x04, sounds)
    cue_name = len(data)
    data += cue.encode() + b"\0"
    name_index = len(data)
    data += struct.pack("<IH", cue_name, 0xFF)
    none = 0xFFFFFFFF
    table = (
        simple_cues,
        none,  # complex cues
        cue_name,
        0,
        none,  # variation tables
        none,  # transitions
        wave_bank_names,
        none,  # cue name hash
        name_index,
        sounds,
    )
    data[offsets_at : offsets_at + 40] = struct.pack("<10I", *table)
    return bytes(data)


def wave_bank(name: str, samples: int = 2205) -> bytes:
    """A wave bank `name` holding one 16-bit mono 22050 Hz PCM wave of
    `samples` samples (2205: 0.1 s), silent."""
    header = b"WBND" + struct.pack("<II", 43, 44)
    bank_data_at = len(header) + 5 * 8
    bank_data = struct.pack("<II", 0, 1) + name64(name) + struct.pack("<IIII", 24, 0, 4, 0)
    bank_data += bytes(8)
    meta_at = bank_data_at + len(bank_data)
    length = samples * 2
    fmt = 0 | (1 << 2) | (22050 << 5) | (2 << 23) | (1 << 31)
    meta = struct.pack("<IIIIII", samples << 4, fmt, 0, length, 0, samples)
    wave_data_at = meta_at + len(meta)
    segments = struct.pack(
        "<10I",
        bank_data_at,
        len(bank_data),
        meta_at,
        len(meta),
        0,
        0,
        0,
        0,
        wave_data_at,
        length,
    )
    return header + segments + bank_data + meta + bytes(length)


def lua_string(path: Path) -> str:
    return "'" + path.as_posix().replace("\\", "/").replace("'", "\\'") + "'"


def init_file(mod_sounds: Path) -> str:
    """Retail's SupComDataPath.lua mounts (FA's folder by the engine's
    fa_path, not by the init file's place), with the mod's sounds folder at
    /sounds first, as FAF's init_faf.lua mounts a mod's ahead of FA's."""
    return (
        "path = {}\n"
        + "local function mount_dir(dir, mountpoint)\n"
        + "    table.insert(path, { dir = dir, mountpoint = mountpoint })\n"
        + "end\n"
        + f"mount_dir({lua_string(mod_sounds)}, '/sounds')\n"
        + "mount_dir(fa_path .. '/gamedata/*.scd', '/')\n"
        + "mount_dir(fa_path, '/')\n"
        + "hook = { '/schook' }\n"
    )


def fa_path(exe: str) -> Path | None:
    """The FA install the engine finds (the gate's retail one), or None."""
    found = subprocess.run(
        [exe, "--print-install"],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    match = re.search(r"^fa_path=(.+)$", found.stdout, re.MULTILINE)
    if found.returncode != 0 or not match:
        return None
    return Path(match.group(1).strip())


def main(argv: list[str]) -> int:
    if len(argv) < 1:
        print(__doc__, file=sys.stderr)
        return 2
    exe = argv[0]
    fa = fa_path(exe)
    if fa is None:
        print("no game data: skipped")
        return SKIPPED
    with tempfile.TemporaryDirectory(prefix="osc-mod-sounds-") as tmp:
        sounds = Path(tmp) / "mods" / "OscModSounds" / "sounds"
        sounds.mkdir(parents=True)
        _ = (sounds / f"{MOD_BANK}.xsb").write_bytes(sound_bank(MOD_BANK, MOD_WAVES, MOD_CUE))
        _ = (sounds / f"{MOD_WAVES}.xwb").write_bytes(wave_bank(MOD_WAVES))
        init = Path(tmp) / "init_mod_sounds.lua"
        _ = init.write_text(init_file(sounds))
        run = subprocess.run(
            [exe, "--init", str(init), "--fa-path", str(fa), "--audio-data-test"],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    output = run.stdout + run.stderr
    if run.returncode == SKIPPED:
        print("no game data: skipped")
        return SKIPPED
    for line in output.splitlines():
        if "Audio data" in line or "Audio:" in line or "FAIL" in line or "PASS" in line:
            print(line)

    problems: list[str] = []
    listed = re.search(r"Audio data: sound banks (\S*)", output)
    banks = listed.group(1).split(",") if listed else []
    if not banks:
        problems.append("no list of the sound banks loaded")
    elif MOD_BANK.lower() not in banks:
        problems.append(f"the mod's bank {MOD_BANK} is not among the {len(banks)} loaded")
    if banks and "interface" not in banks:
        problems.append("FA's own Interface bank is not loaded")
    if "[PASS] every cue resolves to playable waves" not in output:
        problems.append("not every cue (the mod's among them) resolves to a playable wave")
    if run.returncode != 0:
        problems.append(f"--audio-data-test failed, exit {run.returncode}")
    for problem in problems:
        print(f"FAIL: {problem}")
    if problems:
        return 1
    print("mod sounds: the mod's bank loads from /sounds with FA's, and its cue resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
