# M227–M228: Packages and First Run — Design

**Status:** proposed 2026-09-29. Roadmap Phase I (M227 Packaging, M228 First run).

## Why now

The engine plays retail and FAF skirmishes, LAN and matchmade games, replays, saves
and mods, but only for someone who builds it from source and runs it from the build
tree. Nothing installs it, nothing says which version it is, and on a machine where
it can't find FA it prints a command-line hint and exits. A player on Linux or a
Steam Deck can't use it. Phase I is the step from "works on the developer's machine"
to "a download a player can run", and it's cheap next to the phases before it.

## What exists

- `--fa-path`, `OSC_FA_PATH`, FAF and Steam detection (`platform/game_install`,
  M175–M182), and `--print-install`.
- The per-user folders, as XDG and Known Folders (`platform/paths`): Config,
  Cache and State are defined but little used.
- A crash handler (`platform/crash_handler`): its report goes to stderr only.
- `build_id()`: `PROJECT_VERSION-<git describe>` at configure time, which replays
  and saves carry.
- The binary links only `libvulkan.so.1` and the C/C++ runtimes on Linux. It reads
  no file of its own at run time; shaders are compiled in.

What's missing: install rules, packages, a release pipeline, `--version`, a changelog,
a way for a player to point the game at FA, and logs somewhere other than the
working directory. (`opensupcom.log` is written to the CWD, which inside an AppImage,
or when started from a desktop menu, may be unwritable or somewhere unexpected.)

## M227a — Version and install rules

- `--version` prints `OpenSupCom <version> (<revision>)` and exits 0, before
  logging starts, so scripts can read it. The startup log line and `--help` use the
  same text; they're hard-coded `v0.1.0` today.
- The version and the revision live in `osc_core` (`core/version`), below the sim,
  so the logger can use them. The sim's `build_id()`, which replays and saves carry,
  becomes `<version>-<revision>` from there.
- The revision (`git describe --always --dirty`) is taken at every build, not only
  at configure time. A configure-time value goes stale as commits land, so two
  different builds could claim one id: bug reports would name the wrong build, and
  a save from another build would be accepted instead of refused. The header is
  rewritten only when the revision changes, so an unchanged revision recompiles
  nothing. Outside a git checkout it's `unknown`.
- `install()` rules: `bin/opensupcom`; on Linux also
  `share/applications/opensupcom.desktop`, and
  `share/icons/hicolor/scalable/apps/opensupcom.svg` (an original geometric icon,
  with nothing from FA's art or branding); `share/doc/opensupcom/` gets README,
  LICENSE and CHANGELOG. The test runners and tools are not installed.
- `CHANGELOG.md` (Keep a Changelog). 0.1.0 summarises the phases so far.
  Versioning follows SemVer, 0.x until the 1.0 criteria (M230). A release bumps
  `project(VERSION)` and vcpkg.json's version together; a lint test checks that they
  agree.
- Test: a CI step installs to a temporary prefix and runs the installed
  `opensupcom --version`.

## M227b — Packages and the release workflow

- `OSC_STATIC_RUNTIME` (default OFF): `-static-libstdc++ -static-libgcc` on Linux,
  and the static MSVC runtime on Windows (vcpkg triplet `x64-windows-static`). This
  gives one self-contained executable per OS. Vulkan's loader stays the system's:
  every driver installs one, and a bundled one would bypass the user's ICDs.
- A `release` preset per OS: Release, static runtime, tests off.
- `tools/package/appimage.sh <build> <out>`: installs to an AppDir, adds AppRun, the
  desktop file and the icon, and runs a pinned `appimagetool` (download checked
  against a sha256) → `OpenSupCom-<ver>-x86_64.AppImage`. CPack also makes a plain
  `.tar.gz`.
- Windows: CPack ZIP → `OpenSupCom-<ver>-win64.zip` (the exe, docs).
- `.github/workflows/release.yml`:
  - on `v*` tags: builds both packages on ubuntu-22.04 (an older glibc, so the
    AppImage runs on older distributions and SteamOS) and windows-2022, runs each
    package's `--version` (`--appimage-extract-and-run`, since CI has no FUSE), and
    uploads them to a **draft** GitHub release. Publishing stays a person's decision.
  - on pull requests that touch packaging: the same, minus the release, so a
    packaging change is proven before a tag needs it.
- Verified locally: build the AppImage, run `--version`, `--print-install`, and a
  short headless skirmish from it against the Steam install.

## M228a — First run: finding FA

- The engine's own settings: `<Config>/opensupcom/settings.json`, holding for now
  `fa_path`. `locate_game_install` gains one step, after the environment and before
  auto-detection: the folder the player chose.
- A windowed start that finds no FA asks, instead of exiting:
  - a native message box ("OpenSupCom needs Supreme Commander: Forged Alliance
    installed…"), then a folder picker;
  - the choice is checked (`gamedata/` holds FA's `.scd` archives, `bin/` has
    `SupComDataPath.lua`). A wrong folder says what's missing and asks again, and
    Cancel exits;
  - the answer is saved, so the next start is silent.
- Native dialogs come from `tinyfiledialogs` (vcpkg, zlib licence): Win32 dialogs on
  Windows, zenity or kdialog on Linux, which covers GNOME, KDE and SteamOS's desktop
  mode. With neither available, it falls back to the current console message.
- The dialogs sit behind a small interface, so tests drive the flow with scripted
  answers: not found → wrong folder → right folder → saved → found silently on the
  next search.
- Headless and CLI runs never prompt.

## M228b — Logs and bug reports

- The log goes to `<State>/opensupcom/logs/opensupcom.log`, and the previous five
  runs are kept (`opensupcom.1.log` …). `--log <file>` overrides it (tests, and
  anyone who wants the old behaviour).
- The crash handler also writes its report to
  `<State>/opensupcom/crashes/crash-<unix time>.txt`. It uses a path prepared when
  the handler is installed, and `open`/`write` only (async-signal-safe).
- `--collect-logs [file.zip]` bundles the logs, crash reports, `settings.json` and a
  `system.txt` (version, build id, OS, and the GPU and driver from the last log)
  with minizip, and prints where it put the file. Game.prefs is left out: it holds
  the player's name.

## Order and exits

| Slice | Exit |
|---|---|
| M227a | `opensupcom --version` from an installed prefix, in CI |
| M227b | A PR touching packaging builds both packages in CI, and each runs `--version`; the local AppImage plays a headless skirmish |
| M228a | Scripted first-run tests; by hand, a start with no FA found asks, and the next start doesn't |
| M228b | A forced crash (`--crash-test`) leaves a report in the State folder; `--collect-logs` makes a zip that holds it |

## Non-goals

- Flatpak, Snap and distro packages (later; the AppImage covers Linux for now).
- An installer or auto-update; code signing (needs a certificate the project doesn't
  have).
- Steam integration beyond documentation (M229).
