# Changelog

All notable changes to OpenSupCom are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Until the 1.0
criteria in `docs/ROADMAP.md` (M230) are met, versions are 0.x, and a minor
version may change saved games, replays and the network protocol.

A release bumps `project(VERSION)` in `CMakeLists.txt` and `version-string` in
`vcpkg.json` together, and renames `[Unreleased]` below to the new version
(`tools/check_version.py` checks that they agree).

## [Unreleased]

Everything so far: there has been no release yet. The first will be 0.1.0.

### Added

- **Engine core:** Supreme Commander: Forged Alliance's own Lua (5.0, as the
  game ships it) runs on a reimplemented engine. The game's archives are read
  through a virtual file system, with retail's and FAF's init files, hooks and
  mods. Units, weapons, projectiles, props, shields, economy, intel, transports,
  construction, and the order queue follow the game's scripts and the
  decompiled engine's rules.
- **Retail and FAF:** unmodified retail FA (Steam) and FAF's game code run from
  the front end through the lobby to a skirmish and its score screen. Retail's
  AI plays full games.
- **Determinism:** one seeded random stream, portable floating-point math, and
  entity order by id. Windows and Linux builds play a game identically, checked
  by per-tick checksums split by domain.
- **Replays and saved games:** every game records itself; replays play back in
  the game or headlessly; saves load through retail's own dialogs.
- **Multiplayer:** lockstep over the game's own lobby (LAN discovery, a
  reliable-UDP lobby for FAF's ICE adapter), desync detection, dropped-player
  handling, pause and game-speed agreement, and GPGNet, so the FAF client can
  launch the engine.
- **Presentation:** a Vulkan renderer with FA's own shading: map lighting and
  sky, terrain strata and decals, water, unit materials and build effects,
  shields, beams, trails and particles, fog of war, strategic icons, bloom,
  and shadows. XACT audio and movies.
- **Interface:** retail's own UI (front end, lobby, in-game interface) on the
  engine's UI controls, with the game's input, camera and key maps.
- **Linux:** native build and run on Linux (Steam's FA install is found
  automatically), alongside Windows.
- **Release engineering (M227a):** `--version`; `cmake --install` lays out the
  game, its documents, and on Linux a desktop entry and icon; the build id that
  replays and saves carry is taken at every build.
- **Packages (M227b):** a Linux AppImage and tarball (the C++ runtime linked in,
  glibc 2.35 and later) and a Windows zip (with its DLLs and the Visual C++
  runtime). A `v*` tag builds and runs them in CI and drafts a GitHub release.
- **First run (M228a):** when the game can't find Forged Alliance, it asks where
  it is (native dialogs) and remembers the answer in its own settings file.
