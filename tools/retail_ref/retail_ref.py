#!/usr/bin/env python3
"""Ask retail Forged Alliance itself: open a screen of its front end and
measure it, for a reference to check the engine against.

retail_ref.py starts retail FA (through Wine where it isn't Windows) with a
hook of its own (tools/retail_ref/hook): it skips the splash, clicks the
front end's buttons by their labels in turn (as the engine's --click does),
runs the given probes, logs what they find and exits. The tool prints those
lines, and with --screenshot captures the game's window first.

    retail_ref.py --click Options --screenshot options.png
    retail_ref.py --probe tools/retail_ref/probes/texture_sizes.lua \\
        --arg textures=/dialogs/options-02/content-box_bmp.dds
    retail_ref.py --probe tools/retail_ref/probes/font_metrics.lua

A probe is a Lua file defining `Probe(log, args)`, run in retail's UI state
once the clicks are done: `log(text)` reports a line, `args` holds the
--arg KEY=VALUE pairs. It may wait (WaitSeconds).

The game runs on a clean profile, as the engine's captures do (the tutorial's
question already answered): the player's
Game.prefs is set aside and put back, checked byte for byte. A game already
running is left alone: the tool refuses to start.

Where: --fa-path, else OSC_FA_PATH (the install, holding bin/ and gamedata/).
Not Windows: --wine, else $WINE, else `wine`; WINEPREFIX as Wine takes it,
with its Z: drive at / (Wine's default) and Microsoft's d3dx9_35 in it
(winetricks d3dx9_35): retail compiles its effects through D3DX, of which
Wine has only a stub. Where there is no desktop, under Xvfb (xvfb-run); a
--screenshot there needs xdotool and ImageMagick.

    retail_ref.py --self-test
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXE = "SupremeCommander.exe"
# The game's command line names it by a path (pgrep -f, POSIX ERE): a shell
# whose own command merely mentions it doesn't count
GAME_PATTERN = r"[\/]SupremeCommander\.exe"
READY = "OSC-REF: ready"
PREFS_REL = Path("Gas Powered Games") / "Supreme Commander Forged Alliance" / "Game.prefs"
CLEAN_PREFS = """profile = {
    current = 1,
    profiles = {
        {
            Name = 'retail_ref',
            -- Seen, as the engine's captures have it (its menu asks once)
            MenuTutorialPrompt = true,
        },
    },
}
version = {
    major = 1
}
"""


def lua_string(text: str) -> str:
    """`text` as a Lua string literal."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def config_lua(
    clicks: list[str], probes: list[str], args: dict[str, str], settle: float, hold: float
) -> str:
    """The hook's /lua/osc_ref_config.lua."""
    pairs = ", ".join(f"[{lua_string(k)}] = {lua_string(v)}" for k, v in sorted(args.items()))
    return (
        f"clicks = {{ {', '.join(lua_string(c) for c in clicks)} }}\n"
        f"probes = {{ {', '.join(lua_string(p) for p in probes)} }}\n"
        f"args = {{ {pairs} }}\n"
        f"settle = {settle}\n"
        f"hold = {hold}\n"
    )


def init_lua(game: str, hook: str, extra: str) -> str:
    """An init file: retail's data only (no user mods or maps), the hook's
    directory and the run's own (its config and probes); paths as the game
    sees them."""
    lines = [
        "-- retail_ref: retail's data, and the hook (tools/retail_ref)",
        "path = {}",
        "local function mount_dir(dir, mountpoint)",
        "    table.insert(path, { dir = dir, mountpoint = mountpoint } )",
        "end",
        f"mount_dir({lua_string(game + chr(92) + 'gamedata' + chr(92) + '*.scd')}, '/')",
        f"mount_dir({lua_string(game)}, '/')",
        f"mount_dir({lua_string(hook)}, '/')",
        f"mount_dir({lua_string(extra)}, '/')",
        "hook = { '/schook', '/osc_ref' }",
        "protocols = { 'http', 'https' }",
    ]
    return "\n".join(lines) + "\n"


def report(log: str) -> tuple[list[str], bool]:
    """The hook's lines in a game log, padding left out, and whether it
    finished without a failure."""
    lines = []
    ready = failed = False
    for raw in log.splitlines():
        at = raw.find("OSC-REF: ")
        if at < 0:
            continue
        line = raw[at:]
        if line.startswith("OSC-REF: pad "):
            continue
        lines.append(line)
        ready = ready or line == READY
        failed = failed or line.startswith("OSC-REF: failed")
    return lines, ready and not failed


class Platform:
    """How the game is started and seen on this OS."""

    def __init__(self, wine: str | None):
        self.windows = os.name == "nt"
        self.wine = None if self.windows else (wine or os.environ.get("WINE") or "wine")

    def game_path(self, path: Path) -> str:
        """`path` as the game takes it: a Windows path; under Wine its Z:
        drive, which Wine maps to / by default."""
        if self.windows:
            return str(path)
        return "Z:" + str(path.absolute()).replace("/", "\\")

    def running(self) -> bool:
        if self.windows:
            out = subprocess.run(
                ["tasklist", "/FI", f"IMAGENAME eq {EXE}"], capture_output=True, text=True
            )
            return EXE.lower() in out.stdout.lower()
        return subprocess.run(["pgrep", "-f", GAME_PATTERN], capture_output=True).returncode == 0

    def prefs_path(self) -> Path:
        if self.windows:
            return Path(os.environ["LOCALAPPDATA"]) / PREFS_REL
        # Where this Wine keeps a user's local application data, as the game
        # finds it: AppData\Local, or Local Settings\Application Data before
        # Wine 7
        out = subprocess.run(
            [self.wine, "cmd", "/c", "echo", "%LOCALAPPDATA%"], capture_output=True, text=True
        )
        lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        local = prefix_path(lines[-1] if lines else "")
        if local is None:
            raise RuntimeError("Wine didn't say where its local application data is")
        return local / PREFS_REL

    def command(self, exe: Path) -> list[str]:
        return [str(exe)] if self.windows else [self.wine, str(exe)]

    def stop(self) -> None:
        if self.windows:
            subprocess.run(["taskkill", "/F", "/IM", EXE], capture_output=True)
        else:
            subprocess.run(["pkill", "-f", GAME_PATTERN], capture_output=True)


def macos_window() -> tuple[str, str] | None:
    """The game's window on macOS: its number and its owner's pid."""
    script = """
    ObjC.import('CoreGraphics');
    var list = ObjC.castRefToObject(
        $.CGWindowListCopyWindowInfo($.kCGWindowListOptionOnScreenOnly, $.kCGNullWindowID));
    var out = '';
    for (var i = 0; i < list.count; i++) {
        var w = list.objectAtIndex(i);
        if ((ObjC.unwrap(w.objectForKey('kCGWindowName')) || '') == 'Forged Alliance') {
            out = ObjC.unwrap(w.objectForKey('kCGWindowNumber')) + ' ' +
                  ObjC.unwrap(w.objectForKey('kCGWindowOwnerPID'));
            break;
        }
    }
    out;
    """
    out = subprocess.run(
        ["osascript", "-l", "JavaScript", "-e", script], capture_output=True, text=True
    ).stdout.split()
    return (out[0], out[1]) if len(out) == 2 else None


def macos_front(pid: str) -> None:
    """Bring the game's window to the front: macOS gives a window behind
    others no frames, and the game's scripts run on its frames."""
    script = (
        "ObjC.import('AppKit'); "
        f"$.NSRunningApplication.runningApplicationWithProcessIdentifier({pid})"
        ".activateWithOptions(3)"
    )
    subprocess.run(["osascript", "-l", "JavaScript", "-e", script], capture_output=True)


def capture(out: Path) -> bool:
    """The game's window into `out` (PNG)."""
    if sys.platform == "darwin":
        window = macos_window()
        if not window:
            return False
        shot = ["screencapture", "-x", "-o", "-l", window[0], str(out)]
        return subprocess.run(shot).returncode == 0
    if os.name == "nt":
        # The window's rectangle, copied from the screen (System.Drawing)
        script = f"""
        Add-Type -AssemblyName System.Drawing
        Add-Type @'
using System; using System.Runtime.InteropServices;
public class W {{
  [StructLayout(LayoutKind.Sequential)] public struct R {{ public int L, T, Rt, B; }}
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
}}
'@
        $p = Get-Process SupremeCommander | Select-Object -First 1
        $r = New-Object W+R
        [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.Rt - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $bmp.Save('{out}', [System.Drawing.Imaging.ImageFormat]::Png)
        """
        return subprocess.run(["powershell", "-NoProfile", "-Command", script]).returncode == 0
    # X11 (Linux under Wine): the window by its title, through ImageMagick
    found = subprocess.run(
        ["xdotool", "search", "--name", "Forged Alliance"], capture_output=True, text=True
    ).stdout.split()
    if not found:
        return False
    return subprocess.run(["import", "-window", found[0], str(out)]).returncode == 0


def wine_prefix() -> Path:
    return Path(os.environ.get("WINEPREFIX", Path.home() / ".wine"))


def prefix_path(windows_path: str, prefix: Path | None = None) -> Path | None:
    """A Wine prefix's C: path as a file here; None if it isn't one."""
    if len(windows_path) < 3 or windows_path[:3].upper() != "C:\\":
        return None
    parts = [p for p in windows_path[3:].split("\\") if p]
    return (prefix or wine_prefix()).joinpath("drive_c", *parts)


def native_dll(data: bytes) -> bool:
    """Whether a DLL's bytes are a real one, not Wine's own (which says so)."""
    return b"Wine builtin DLL" not in data


def wine_d3dx_problem(prefix: Path) -> str | None:
    """What keeps retail from starting in a Wine prefix, if anything: its
    effects compile through D3DX, whose d3dx9_35 Wine has only a stub of."""
    windows = prefix / "drive_c" / "windows"
    # A 32-bit game's DLLs: syswow64 in a 64-bit prefix, else system32
    dll = windows / ("syswow64" if (windows / "syswow64").is_dir() else "system32") / "d3dx9_35.dll"
    if dll.is_file() and native_dll(dll.read_bytes()):
        return None
    return (
        f"{dll} isn't Microsoft's: retail can't compile its effects with Wine's own"
        " (it stops at /effects/cartographic.fx). Install it: winetricks d3dx9_35"
    )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(opts: argparse.Namespace) -> int:
    fa = Path(opts.fa_path or os.environ.get("OSC_FA_PATH", ""))
    exe = fa / "bin" / EXE
    if not exe.is_file():
        print(f"retail FA not found: no {exe} (give --fa-path or OSC_FA_PATH)")
        return 2
    plat = Platform(opts.wine)
    if not plat.windows:
        problem = wine_d3dx_problem(wine_prefix())
        if problem:
            print(problem)
            return 2
    if plat.running():
        print(f"{EXE} is running: close it first (this run would stop it)")
        return 2

    bad = [a for a in opts.arg if "=" not in a]
    if bad:
        print(f"--arg {bad[0]}: give it as KEY=VALUE")
        return 2
    args = dict(a.split("=", 1) for a in opts.arg)
    work = Path(tempfile.mkdtemp(prefix="retail_ref_", dir=opts.work))
    (work / "lua").mkdir()
    probes = []
    for i, probe in enumerate(opts.probe):
        name = f"osc_ref_probe_{i}"
        shutil.copyfile(probe, work / "lua" / f"{name}.lua")
        probes.append(name)
    hold = 15.0 if opts.screenshot else 1.0
    (work / "lua" / "osc_ref_config.lua").write_text(
        config_lua(opts.click, probes, args, opts.settle, hold)
    )
    log = work / "game.log"
    init = work / "init.lua"
    init.write_text(
        init_lua(plat.game_path(fa), plat.game_path(HERE / "hook"), plat.game_path(work))
    )

    prefs = Path(opts.prefs) if opts.prefs else plat.prefs_path()
    saved = work / "Game.prefs.saved"
    had_prefs = prefs.exists()
    if had_prefs:
        shutil.copy2(prefs, saved)
    digest = sha256(saved) if had_prefs else None
    width, height = opts.size.split("x")
    lines: list[str] = []
    ok = False
    try:
        prefs.parent.mkdir(parents=True, exist_ok=True)
        prefs.write_text(CLEAN_PREFS)
        cmd = plat.command(exe) + [
            "/init", plat.game_path(init), "/nosound", "/windowed", width, height,
            "/nobugreport", "/nomovie", "/log", plat.game_path(log),
        ]  # fmt: skip
        with open(work / "stdout.txt", "w") as out:
            game = subprocess.Popen(cmd, cwd=exe.parent, stdout=out, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + opts.timeout
        fronted = False
        while time.monotonic() < deadline and game.poll() is None:
            if sys.platform == "darwin" and not fronted:
                window = macos_window()
                if window:
                    macos_front(window[1])
                    fronted = True
            text = log.read_text(errors="replace") if log.exists() else ""
            if READY in text:
                break
            time.sleep(1)
        if opts.screenshot and log.exists() and READY in log.read_text(errors="replace"):
            time.sleep(1)
            if not capture(Path(opts.screenshot)):
                print("the game's window couldn't be captured")
        try:
            game.wait(timeout=max(5.0, hold + 20))
        except subprocess.TimeoutExpired:
            plat.stop()
        lines, ok = report(log.read_text(errors="replace") if log.exists() else "")
    finally:
        if had_prefs:
            shutil.copy2(saved, prefs)
            restored = sha256(prefs) == digest
            if restored:
                print("Game.prefs restored")
            else:
                print(f"Game.prefs NOT restored: the original is {saved}")
        else:
            prefs.unlink(missing_ok=True)
    for line in lines:
        print(line)
    if not ok:
        print(f"no answer from retail: see its log, {log}")
        return 1
    if not opts.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 0


def self_test() -> int:
    failed = 0

    def check(name: str, got: object, want: object) -> None:
        nonlocal failed
        if got != want:
            print(f"self-test FAILED: {name}: {got!r}, expected {want!r}")
            failed += 1

    check("a Lua string", lua_string('a "b" \\ c'), '"a \\"b\\" \\\\ c"')
    config = config_lua(["Skirmish", 'Game "Options"'], ["osc_ref_probe_0"], {"k": "v"}, 3, 1)
    check(
        "the config",
        config,
        'clicks = { "Skirmish", "Game \\"Options\\"" }\n'
        'probes = { "osc_ref_probe_0" }\n'
        'args = { ["k"] = "v" }\n'
        "settle = 3\nhold = 1\n",
    )
    init = init_lua("C:\\games\\FA", "Z:\\tools\\retail_ref\\hook", "Z:\\tmp\\run")
    check(
        "the init's game data",
        "mount_dir(\"C:\\\\games\\\\FA\\\\gamedata\\\\*.scd\", '/')" in init,
        True,
    )
    check("the init's hook", "hook = { '/schook', '/osc_ref' }" in init, True)
    if os.name != "nt":
        check("a path under Wine", Platform("wine").game_path(Path("/tmp/a b")), "Z:\\tmp\\a b")
    log = "\n".join(
        [
            "info: OSC-REF: main menu",
            "info: OSC-REF: clicked Options",
            "info: OSC-REF: probe a 602x34",
            "info: OSC-REF: ready",
            "info: OSC-REF: pad 1",
        ]
    )
    check(
        "a log's report",
        report(log),
        (
            [
                "OSC-REF: main menu",
                "OSC-REF: clicked Options",
                "OSC-REF: probe a 602x34",
                "OSC-REF: ready",
            ],
            True,
        ),
    )
    check("a failed run", report("OSC-REF: failed: no button X\nOSC-REF: ready")[1], False)
    check("Microsoft's DLL", native_dll(b"MZ...Microsoft (R) D3DX..."), True)
    check(
        "Wine's local data",
        prefix_path("C:\\users\\a\\Local Settings\\Application Data", Path("/p")),
        Path("/p/drive_c/users/a/Local Settings/Application Data"),
    )
    check("not a C: path", prefix_path("%LOCALAPPDATA%", Path("/p")), None)
    check("Wine's own DLL", native_dll(b"MZ...Wine builtin DLL..."), False)
    check("a run that never got ready", report("OSC-REF: main menu")[1], False)
    if failed == 0:
        print("self-test passed")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--fa-path", help="the retail install (else OSC_FA_PATH)")
    p.add_argument("--click", action="append", default=[], help="a button's label, in turn")
    p.add_argument("--probe", action="append", default=[], help="a probe's Lua file, in turn")
    p.add_argument("--arg", action="append", default=[], help="KEY=VALUE for the probes")
    p.add_argument("--screenshot", help="capture the game's window into this PNG")
    p.add_argument("--size", default="1600x900", help="the window's size (default 1600x900)")
    p.add_argument("--settle", type=float, default=3, help="seconds after the clicks")
    p.add_argument("--timeout", type=float, default=180, help="seconds to wait for retail")
    p.add_argument("--wine", help="Wine's command where not Windows (else $WINE, wine)")
    p.add_argument("--prefs", help="Game.prefs to set aside (else where the game keeps it)")
    p.add_argument("--keep", action="store_true", help="keep the run's folder (its log)")
    p.add_argument("--work", help="where to make the run's folder (else the temp folder)")
    return run(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
