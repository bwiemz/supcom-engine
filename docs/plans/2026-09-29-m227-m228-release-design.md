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
- `CHANGELOG.md` (Keep a Changelog). `[Unreleased]` summarises the work so far;
  the first release will be 0.1.0.
  Versioning follows SemVer, 0.x until the 1.0 criteria (M230). A release bumps
  `project(VERSION)` and vcpkg.json's version together; a lint test checks that they
  agree.
- Test: a CI step installs to a temporary prefix and runs the installed
  `opensupcom --version`.

## M227b — Packages and the release workflow

- `OSC_STATIC_RUNTIME` (default OFF): `-static-libstdc++ -static-libgcc` on Linux.
  The game then asks a player's system for only glibc and the Vulkan loader. Vulkan's
  loader stays the system's: every driver installs one, and a bundled one would
  bypass the user's ICDs.
- Windows keeps the dynamic vcpkg triplet CI already builds and caches. A static
  triplet would mean a second full dependency build. Instead, the install puts the
  vcpkg DLLs (M227a) and the Visual C++ runtime (`InstallRequiredSystemLibraries`)
  beside the game, so no redistributable needs installing. Windows 10 and later carry
  the universal CRT.
- `tools/package/appimage.sh <build> <out>`: installs to an AppDir, adds AppRun, the
  desktop file and the icon, and packs it with a pinned `appimagetool` (1.9.1) and
  type-2 runtime (20251108). Each download is checked against its sha256, and the
  runtime's signature was verified against its release key when it was pinned. The
  result is `OpenSupCom-<ver>-x86_64.AppImage`. CPack also makes a plain
  `OpenSupCom-<ver>-linux-x86_64.tar.gz`.
- Windows: CPack ZIP → `OpenSupCom-<ver>-win64.zip` (the exe, its DLLs, the docs).
- `tools/package/check_linux_package.sh <dist>` runs both Linux packages' `--version`.
  It also checks that the game needs nothing from the system beyond the Vulkan loader
  and glibc, and no glibc newer than 2.35 (Ubuntu 22.04's).
- `.github/workflows/release.yml`:
  - on `v*` tags: builds both packages on ubuntu-22.04 (an older glibc, so the
    AppImage runs on older distributions and SteamOS) and windows-2022, runs each
    package's `--version` (`--appimage-extract-and-run`, since CI has no FUSE), and
    uploads them to a **draft** GitHub release. Publishing stays a person's decision.
  - on pull requests that touch packaging: the same, minus the release, so a
    packaging change is proven before a tag needs it.
- Verified locally: build the AppImage, run `--version`, `--print-install`, and a
  short headless skirmish from it against the Steam install. CI builds on 22.04 with
  GCC 13 from the toolchain PPA (22.04's own GCC 11 is older than any compiler CI
  tests the project with); the static C++ runtime keeps GCC 13's libstdc++ from
  becoming a requirement.

## M228a — First run: finding FA

- The engine's own settings: `<Config>/opensupcom/settings.json` (`platform/engine_settings`),
  holding for now `fa_path`. Keys another version wrote are kept when it saves, and
  the file is replaced whole (a temporary file, then a rename).
- `locate_game_install` gains a last step: the folder the player chose. It is last,
  not first, because it is only ever asked for when nothing was found, and an install
  found later (FAF, or Steam after a reinstall) should take over. `--fa-path` and
  `OSC_FA_PATH` still win over everything, and `--print-install` lists it.
- A player's windowed start that finds no FA asks, instead of exiting:
  - a question ("OpenSupCom plays Supreme Commander: Forged Alliance from your own
    copy…"), then a folder picker;
  - the choice is checked (`fa_install_problem`: `bin/SupComDataPath.lua` and
    `gamedata/lua.scd`, in any case). FA's own `bin` or `gamedata` folder counts as
    FA. A wrong folder says what it lacks and offers another try; Cancel exits with
    the old message;
  - the answer is saved, so the next start is silent. When the save fails, the game
    still plays and says so, and the next start asks again.
- Native dialogs come from `portable-file-dialogs` (vcpkg, one header, fetched from
  GitHub). `tinyfiledialogs`' port downloads from SourceForge, a flakier dependency
  for CI. pfd uses Windows' own dialogs, and on Linux zenity, matedialog, qarma or
  kdialog, which covers GNOME, KDE and SteamOS's desktop mode.
  - With none of those installed, pfd 0.1.0 "shows" a dialog by running `echo`, and
    a folder pick would return echo's arguments as the folder. So the engine checks
    for a helper on `PATH` and a display itself, and without them keeps the console
    message.
  - kdialog waits forever for an X server that isn't there. Only a broken display
    reaches that, and the game can't run there anyway.
- The dialogs sit behind `platform::Prompter`, so tests drive the flow with scripted
  answers: cancel at each step; wrong folder, then FA's bin folder → FA, saved;
  broken or foreign settings files. It was also run end to end with a scripted
  `zenity`/`kdialog` on `PATH` and an empty `HOME`: question → folder → saved → the
  search finds it as `chosen` → the next start asks nothing.
- Headless runs, test modes and captures never ask.

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
