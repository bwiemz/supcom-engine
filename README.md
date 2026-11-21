# OpenSupCom

An open-source reimplementation of the Moho engine that powers *Supreme Commander: Forged Alliance*.

## Project Goal

OpenSupCom aims to be a **1:1 compatible reimplementation** of the Supreme Commander: Forged Alliance engine (the "Moho" engine). The goal is to run unmodified FA game data and Lua scripts — including the full [FAForever](https://www.faforever.com/) mod stack — against a clean, modern C++ codebase.

This is **not** a compatibility layer or wrapper around the original binary. It is a ground-up rewrite that reimplements the engine's internal systems: the Lua VM bridge, simulation tick loop, entity lifecycle, economy, construction, combat, and AI — so that the existing FA Lua gameplay code runs without modification.

### Why?

The original Moho engine is closed-source, 32-bit, single-threaded, and increasingly difficult to maintain. A clean reimplementation opens the door to:

- **64-bit and cross-platform support** (Windows, Linux and macOS on Apple Silicon)
- **Modern C++ performance** (C++20, no legacy COM/DirectX constraints in the sim)
- **Debuggability** (full source, structured logging, deterministic replay)
- **Community extension** (open codebase for FAForever and modders)

## At a glance

**What works today:**

- **Skirmish against FA's AI**, on the retail game's data or on FAForever's. Steam's install is found on its own, and any other by its folder. Retail's own front end and lobby set the game up, and it plays through to the score screen.
- **FA's own interface in the game:** the orders and construction panels, selection, the minimap, build templates, the key map and the console.
- **The campaign:** from the operation select through the briefing, the operation and its score to the next briefing, and the tutorial and the outro.
- **Multiplayer:** LAN games (find, host and join a lobby) and FAForever's client protocol (GPGNet), played in lockstep. Desyncs are detected, a dropped or slow player is handled, and chat, pause and game speed work. Windows and Linux players can play together.
- **Saves and replays:** save a game and load it to play on, campaign saves included; record a game and play its replay back.
- **Mods:** game and UI mods, hooks and the player's mods folder, loaded as the original engine loads them. `opensupcom --check-mods` reports what is broken in a mod, by file and line.
- **The game's systems, as FA's scripts drive them:**
  - combat: weapons, projectiles, missiles and their defences, shields, armour and veterancy;
  - movement on the original engine's pathing, by each unit's footprint;
  - aircraft: attack runs, landing and refuelling, transports and carriers;
  - construction and economy: adjacency, upgrades, reclaim and capture;
  - intel and fog of war.
- **Graphics and sound from FA's own data:** terrain, water, sky, units, decals, shadows, effects and strategic icons drawn with FA's shaders, the range overlays, and FA's sounds, voices and music.
- **Windows and Linux,** with a zip for Windows and an AppImage (glibc 2.35+, such as Ubuntu 22.04 or SteamOS) and a tarball for Linux. It also builds on macOS on Apple Silicon, where CI runs its data-free tests.
- **Tests:** CI runs the unit tests on GCC, Clang, ASan, MSVC and macOS, and a gate of tests plays retail's scripts and data. The counts, and the test behind each claim above, are in [docs/current-state.md](docs/current-state.md).

**Not yet:**

- **A release for players:** packages are built, but launching through Steam (with the Steam Deck) and the 1.0 criteria are still open. See [Phase I](docs/ROADMAP.md) in the roadmap.
- **Hardened multiplayer:** peers are not authenticated, two players dropping at once can leave the others disagreeing, and FAForever's ICE adapter and client are tested only through stand-ins. See [Phase G](docs/ROADMAP.md).
- **The original engine's simulation, bit for bit:** some movement still parts from it in a float's last bits, as Moho's x87 math isn't reproduced. See [Known gaps](docs/current-state.md#known-gaps).
- **A threaded renderer and GPU-driven instancing,** the open part of the performance work. See [Phase H](docs/ROADMAP.md).
- **Checks that need a person:** Steam and the Steam Deck, and a full side-by-side look against the original game.

What works and how each part is checked: [docs/current-state.md](docs/current-state.md). The plan: [docs/ROADMAP.md](docs/ROADMAP.md). The early milestones: [docs/milestones.md](docs/milestones.md).

## Prerequisites

- **Supreme Commander: Forged Alliance.** The retail game (Steam/GOG) is
  enough. A FAForever install is used automatically if present.
- **CMake 3.21+**, **Ninja** (Linux), and **vcpkg** with `VCPKG_ROOT` set. vcpkg
  installs every C++ dependency from `vcpkg.json`.
- **A Vulkan 1.2 driver.** Without one the engine still runs headless.
- **Windows 10/11:** Visual Studio 2022 with the C++ desktop workload.
- **Linux:** GCC 13+ or Clang 17+, `git curl zip unzip tar pkg-config`, and
  X11/Wayland development headers for GLFW (on Debian/Ubuntu:
  `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
  libxkbcommon-dev libwayland-dev wayland-protocols`).

## Building

```bash
git clone https://github.com/bwiemz/supcom-engine.git
cd supcom-engine
```

**Linux**

```bash
cmake --preset linux-debug          # or linux-release, linux-asan (ASan+UBSan)
cmake --build build/linux-debug
# -> build/linux-debug/opensupcom, build/linux-debug/tests/osc_tests
```

**Windows**

```bash
cmake --preset default
cmake --build build --config Debug
# -> build/Debug/opensupcom.exe, build/tests/Debug/osc_tests.exe
```

**Installing** lays out the game and its documents under a prefix (on Linux,
with a desktop entry and icon; on Windows flat, with the DLLs it needs), and
`--version` says which build it is:

```bash
cmake --install build/linux-release --prefix ~/.local
opensupcom --version                 # OpenSupCom 0.1.0 (<git revision>)
```

Changes are listed in [CHANGELOG.md](CHANGELOG.md).

**Packages** (what a release carries; `.github/workflows/release.yml` builds
them on a `v*` tag):

```bash
# Linux: an AppImage (runs on glibc 2.35+, e.g. Ubuntu 22.04, SteamOS) and a tarball
cmake --preset linux-release -DOSC_BUILD_TESTS=OFF -DOSC_STATIC_RUNTIME=ON
cmake --build build/linux-release --target opensupcom
tools/package/appimage.sh build/linux-release dist
(cd build/linux-release && cpack -G TGZ -B ../../dist)
tools/package/check_linux_package.sh dist   # MAX_GLIBC=<yours> on a newer distro

# Windows: a zip with the game, its DLLs and the Visual C++ runtime
cmake --preset default -DOSC_BUILD_TESTS=OFF
cmake --build build --config Release --target opensupcom
cd build && cpack -G ZIP -C Release -B ../dist
```

### Dependencies (managed by vcpkg)

| Package | Purpose |
|---------|---------|
| zlib | Compression (VFS archive reading) |
| minizip | .scd/.nx2 archive extraction |
| spdlog | Structured logging |
| fmt | String formatting |
| catch2 | Unit test framework |
| glfw3 | Window management (Vulkan surface creation) |
| vulkan-headers | Vulkan API headers |
| vulkan-loader | Vulkan runtime loader |
| vulkan-memory-allocator | GPU memory management (VMA) |
| vk-bootstrap | Vulkan instance/device/swapchain setup |
| shaderc | Runtime GLSL to SPIR-V shader compilation |

Vendored under `third_party/`:

- **Lua 5.0** (`third_party/lua-5.0/`): the LuaPlus dialect FA's scripts use,
  with `!=`, `continue`, `#` comments, table size hints and per-type
  metatables. It is built as C++ on GCC/Clang so Lua errors unwind C++ frames.
- **miniaudio** (audio).
- **pl_mpeg** (movies).
- **stb** (fonts, images).

## Running

The engine needs FA game data and finds it on its own. The search order is:

1. `--init` / `--fa-path` / `--faf-data`
2. The environment variables `OSC_INIT_FILE`, `OSC_FA_PATH` and `OSC_FAF_DATA`
3. A FAForever data directory (`%ProgramData%/FAForever` or `~/.faforever`)
4. Steam app 9420, in any Steam library, using the retail
   `bin/SupComDataPath.lua` init script
5. The folder you chose: when nothing else is found, the game asks where FA is
   installed (native dialogs; on Linux through zenity or kdialog) and keeps the
   answer in `~/.config/opensupcom/settings.json` (`%LOCALAPPDATA%` on Windows,
   `~/Library/Application Support/opensupcom` on macOS)

The tests labelled `gate` set `OSC_RETAIL_ONLY=1`, a test switch: step 3 then takes only
the FA folder that FAForever's `fa_path.lua` names, with retail's own init script, and step 2
ignores `OSC_INIT_FILE` and `OSC_FAF_DATA`.

To see what it picked and everything it checked, run:

```bash
./build/linux-debug/opensupcom --print-install
```

The examples below use the Linux binary path. On Windows use
`build/Debug/opensupcom.exe`, and when running under Git Bash prefix
commands with `MSYS_NO_PATHCONV=1` so `/maps/...` isn't rewritten.

### Windowed Mode (Renderer)

When launched without `--ticks` or test flags, the engine opens a Vulkan window with real-time rendering:

```bash
# Open a window showing Seton's Clutch with terrain, units, and water
./build/linux-debug/opensupcom --map "/maps/SCMP_009/SCMP_009_scenario.lua"

# Play a skirmish against a rush AI with cheat difficulty
./build/linux-debug/opensupcom --map "/maps/SCMP_009/SCMP_009_scenario.lua" \
  --ai-personality rushcheat
```

**AI personalities:** `adaptive`, `rush`, `turtle`, `tech`, `random`, and their
cheat variants `adaptivecheat`, `rushcheat`, `turtlecheat`, `techcheat`,
`randomcheat`. Cheat personalities get 2x build rate and 2x income.

Camera controls:
- **Arrow keys**, or the mouse at the window's edge — Pan camera (speed scales
  with zoom distance; not while a text box has the keyboard)
- **Mouse scroll** — Zoom in/out
- **Middle mouse drag** — Orbit camera

The other keys are FA's own, from its key map (`keyNames.lua`,
`defaultKeyMap.lua`): Esc for the menu, 1–0 and Ctrl-1 for control groups,
M/A/S for move, attack and stop, Pause, NumPlus/NumMinus for game speed, and
so on. Close the game from its menu or the window.

The simulation runs at 10 Hz (fixed timestep) decoupled from the render framerate.

### Headless Mode

Adding `--ticks N` runs the simulation headlessly for N ticks with no window:

```bash
# Run 100 sim ticks on Seton's Clutch (headless)
./build/linux-debug/opensupcom --map "/maps/SCMP_009/SCMP_009_scenario.lua" --ticks 100

# Run the AI test (ARMY_2 builds a base autonomously, 1200 ticks). Test modes
# are in the integration runner, built beside the game.
./build/linux-debug/osc_integration --map "/maps/SCMP_009/SCMP_009_scenario.lua" --ticks 1200 --ai-test
```

### Checking a Mod

The game says little about a broken mod: one whose `mod_info.lua` fails just
leaves the mod manager, and a hook of a misspelt path never runs. `--check-mods`
reads the installed mods as a game does, without a window, and reports what is
wrong, file and line:

```bash
opensupcom --check-mods                       # every installed mod
opensupcom --check-mods ~/Documents/My\ Games/Gas\ Powered\ Games/Supreme\ Commander\ Forged\ Alliance/mods/MyMod
opensupcom --check-mods <uid> --json          # a mod by its uid; the report as JSON
opensupcom --list-mods                        # what is installed
```

It checks that `mod_info.lua` runs, with a unique uid, a whole-number version
and a `hookdir` from `/`; that the mods it requires are installed and those it
conflicts with aren't; that each hook hooks a file there is and compiles with
it; that each blueprint compiles and runs, has a `ShaderName` the renderer
knows, and names mesh and texture files that exist; and that its sound banks
parse. It exits 1 on an error and 0 on warnings alone.

It runs a mod's `mod_info.lua` and blueprints as the game does, but without
Lua's `io`, `dofile` and `loadfile`, so checking a mod you don't yet trust
gives its scripts no access to your files.

## Testing

| Suite | Command | Needs game data |
|---|---|---|
| Unit tests (Catch2) | `ctest --preset linux-debug -LE data` or run `osc_tests` directly | No |
| Two-process multiplayer (lockstep sync, desync detection, LAN lobby, peer drop) | `ctest --preset linux-debug -L mp` | No |
| Data-backed regression gate | `ctest --preset linux-debug -L gate` | Yes |
| All data-backed modes, including known retail gaps | `ctest --preset linux-debug -L data` | Yes |
| Sim benchmark: a pinned four-AI game held to this machine's baseline (Release; `tools/bench.py`) | `ctest --test-dir build/linux-release -L bench` | Yes |

CI (GitHub Actions) builds on GCC, Clang, ASan+UBSan, MSVC and macOS (arm64) and
runs every data-free test. The `--<name>-test` integration modes are in the integration
runner, `osc_integration` (built beside `opensupcom`, which doesn't ship them;
`osc_integration --help` lists them). They exit non-zero when a check fails
and with 77 (skipped) when no game data is available. Their
`gate` / `retail-gap` membership lives in `tests/integration/data_tests.cmake`.

### Integration Test Flags

| Flag | Description |
|------|-------------|
| `--damage-test` | Damage pipeline and unit death |
| `--move-test` | Movement orders and navigator |
| `--fire-test` | Weapons and projectile combat |
| `--economy-test` | Resource income, consumption, storage |
| `--build-test` | Construction and build progress |
| `--chain-test` | ACU builds factory, factory builds engineer, engineer builds pgen |
| `--ai-test` | Full AI base building with guard/assist (16+ entities) |
| `--reclaim-test` | Prop reclaim system |
| `--platoon-test` | Platoon creation, commands, disbanding |
| `--threat-test` | Threat queries and platoon targeting |
| `--combat-test` | AI army production and HuntAI attack loop |
| `--repair-test` | Build, damage, and repair a structure |
| `--upgrade-test` | T1 mex to T2 mex structure upgrade |
| `--capture-test` | Capture enemy unit and verify army transfer |
| `--path-test` | A* pathfinding around obstacles |
| `--toggle-test` | Script bits, toggle caps, and dive command |
| `--enhance-test` | ACU enhancement (AdvancedEngineering) |
| `--intel-test` | Intel system (init, enable, disable, radius) |
| `--shield-test` | Shield system (create, health, regen, toggle) |
| `--transport-test` | Transport load/unload, cargo tracking, speed mult |
| `--fow-test` | Fog of war visibility grid and OnIntelChange callbacks |
| `--los-test` | Terrain LOS occlusion (ridge blocking, Bresenham ray) |
| `--stall-test` | Economy stalling (efficiency scaling, resource rationing) |
| `--jammer-test` | Radar jamming, dead-reckoning, stealth, IsKnownFake |
| `--stub-test` | Moho stub conversions (14 real implementations) |
| `--audio-test` | Audio system (XWB/XSB parsing, PlaySound, 3D spatial) |
| `--bone-test` | Bone system (SCM parser, bone queries, ShowBone/HideBone) |
| `--manip-test` | Manipulator system (rotators, animators, sliders, aim, WaitFor) |
| `--canpath-test` | CanPathTo pathfinding queries and GetThreatBetweenPositions |
| `--armor-test` | Armor types and damage multipliers |
| `--vet-test` | Veterancy system (regen, vet XP, level up) |
| `--wreck-test` | Wreckage system (SetMaxReclaimValues, GetHeading) |
| `--adjacency-test` | Adjacency bonus system and SetFiringRandomness |
| `--stats-test` | Stats/telemetry (SetStat/GetStat/UpdateStat) |
| `--silo-test` | Missile silo ammo (Give/Remove/Get nuke+tactical) |
| `--flags-test` | Unit targeting flags (DoNotTarget, Reclaimable, IsValidTarget) |
| `--layercap-test` | Weapon fire target layer caps |
| `--massstub-test` | Mass stub conversion I (weapon/movement/fuel/projectile/misc) |
| `--massstub2-test` | Mass stub conversion II (damage flags/caps/weapon/proj/elevation) |
| `--massstub3-test` | Mass stub conversion III (brain/weapon/projectile/platoon) |
| `--massstub4-test` | Mass stub conversion IV (visibility/scale/mesh/collision/attach) |
| `--anim-test` | SCA skeletal animation (parsing, bone matrices, GPU skinning) |
| `--teamcolor-test` | Team color rendering (SpecTeam texture resolution, DDS validation) |
| `--normal-test` | Normal map rendering (path resolution, DDS validation, tangent data) |
| `--prop-test` | Map prop rendering (SCMAP parsing, 5,182 props, orientation) |
| `--scale-test` | Prop scale and distance culling |
| `--specular-test` | Blinn-Phong specular lighting |
| `--terrain-tex-test` | Terrain textures (stratum blending, blend maps, UV scaling) |
| `--terrain-normal-test` | Terrain normal maps (per-stratum DXT5nm, TBN, blending) |
| `--decal-test` | Terrain decal rendering (2,236 decals, alpha blend, LOD culling) |
| `--projectile-test` | Projectile mesh rendering with velocity-aligned orientation |
| `--shadow-test` | Shadow mapping (depth pass, light matrix, shadow sampling) |
| `--spatial-test` | Spatial hash grid (grid init, collect_in_radius/rect, auto-notify) |
| `--unitsound-test` | Unit sound (PlayUnitSound, PlayUnitAmbientSound) |
| `--medstub-test` | Medium stubs (SetBoneEnabled, AddOnGivenCallback) |
| `--lowstub-test` | Low-priority stubs (Destroy/BeenDestroyed, CreateBuilderArmController) |
| `--blend-test` | Blend-weight skinning (multi-bone parsing, weight validation) |
| `--ui-test` | UI control system (Frame, Group, LazyVar, moho bindings) |
| `--bitmap-test` | Bitmap control (SetNewTexture, solid color, UVs, animation) |
| `--text-test` | Text control (SetNewFont, SetText, font metrics, centering) |
| `--edit-test` | Edit/ItemList/Scrollbar controls (text input, list ops, scroll) |
| `--controls-test` | Border/Dragger/Cursor/Movie/Histogram/WorldMesh controls |
| `--uiboot-test` | UI bootstrap (GetFrame, WorldView, WldUIProvider, lobby/discovery) |
| `--uirender-test` | UI Vulkan 2D rendering pipeline (instanced quads, texture batching) |
| `--font-test` | Font rendering (stb_truetype atlas, glyph metrics, drop shadow) |
| `--scissor-test` | Scissor/clip rectangles (dynamic scissor, clip rect intersection) |
| `--border-render-test` | Border 9-patch rendering (corners, edges, DDS dimensions) |
| `--edit-render-test` | Edit control visuals (caret, selection highlight, cursor positioning) |
| `--itemlist-render-test` | ItemList rendering (rows, scroll offset, selection highlight) |
| `--scrollbar-render-test` | Scrollbar rendering (thumb position, 3-piece textures) |
| `--anim-render-test` | Bitmap animation rendering (frame timing, patterns, loop/stop) |
| `--tiled-render-test` | Tiled bitmap rendering (UV repeat, sampler wrap mode) |
| `--input-test` | Input event dispatch (hit-test, HandleEvent, keyboard focus) |
| `--onframe-test` | OnFrame update loop (delta time, NeedsFrameUpdate) |
| `--cursor-render-test` | Cursor rendering (texture at mouse position, topmost depth) |
| `--drag-render-test` | Drag visual feedback (dragger intercept, OnMove/OnRelease/OnCancel) |
| `--emitter-test` | VFX/emitter system (IEffect creation, parameters, lifecycle) |
| `--collision-test` | CollisionBeam entity system (enable/disable, endpoints) |
| `--decalsplat-test` | Decal/Splat system (CreateDecal, CreateSplat, IEffect tracking) |
| `--cmd-test` | Issue commands + economy events (6 command types from Lua) |
| `--deposit-test` | Resource deposits (mass/hydrocarbon entities, Lua queries) |
| `--beam-test` | Beam rendering (operation beams + CollisionBeam weapon beams) |
| `--shield-render-test` | Shield bubble rendering (projected circles, HP-based alpha) |
| `--mesh-capacity-test` | Every mesh in view drawn past 8,192 instances; instance and bone buffers grow; none built at strategic zoom |
| `--vet-adj-render-test` | Veterancy indicators + adjacency lines |
| `--intel-overlay-test` | Intel range overlay (radar/sonar/omni/vision circles) |
| `--enhance-wreck-test` | Enhancement mesh switching + wreckage desaturation |
| `--vfx-render-test` | VFX particle rendering (emitter billboards, beam lines) |
| `--transport-silo-test` | Transport cargo dots + nuke/tactical silo ammo indicators |
| `--profile-test` | Profiler system (zones, nesting, rolling stats) |
| `--smoke-test` | Smoke test harness (method/global interceptors, 100-tick/100-frame run) |
| `--ai-skirmish` | AI-vs-AI skirmish (2 AI armies, full game loop, 6000 ticks) |
| `--stress-test` | Extended AI-vs-AI stress test (10,000 ticks, stability validation) |
| `--draw-test` | Simultaneous ACU death → Draw game-over edge case |
| `--full-smoke-test` | 5-phase lifecycle smoke test: front-end → lobby → 1000-tick game → score → return-to-lobby |
| `--lobby-flow-test` | Headless front-end `ButtonSkirmish()` to hosted lobby callback smoke |
| `--instrument` | Silent smoke harness during interactive play, report on exit to smoke_report.txt |

## Project Structure

```
src/
  app/         # The game's run: boot, session, CLI flags, headless and frame loops, benchmarks
  core/        # Fundamental types (Vector3, numeric aliases)
  vfs/         # Virtual filesystem (.scd/.nx2 archive mounting)
  map/         # Map loading (.scmap terrain, scenario files, A* pathfinding, visibility grid)
  sim/         # Simulation (Entity, Unit, Projectile, Shield, Platoon, ArmyBrain, economy, intel, bones, manipulators)
  lua/         # Lua<->C++ bridge (moho bindings, sim bindings, session management)
  blueprints/  # Blueprint loading and registry
  audio/       # Audio system (miniaudio, XWB/XSB bank parsers, sound manager)
  video/       # FA's movies (MPEG decoding, movie player)
  platform/    # OS services (game install and Steam library lookup, settings, paths, first run, crash handler)
  ui/          # UI control system (UIControl base, UIControlRegistry, input dispatch, font metrics)
  renderer/    # Vulkan renderer (terrain mesh, SCM mesh units, water plane, camera, shaders, mesh cache, UI 2D pipeline, font atlas, HUD, minimap, overlays, strategic icons, selection info, input handler)
  main.cpp     # Entry point, handing over to src/app
third_party/
  lua-5.0/     # Vendored Lua 5.0 (LuaPlus fork)
  stb/         # stb_truetype for font rendering
tests/         # Catch2 unit tests; the integration runner osc_integration and its test modes (tests/integration)
tools/         # Formatting check and clang-tidy ratchet (see CONTRIBUTING.md)
```

## Questions and Discussion

[Discussions](https://github.com/bwiemz/supcom-engine/discussions) is the place
for questions (Q&A), ideas to talk over before they become issues (Ideas), and
reports of how a map, mod, campaign operation or replay runs (Compatibility
reports). Issues are for bugs and planned work.

## Reporting Bugs

A player's game logs to `~/.local/state/opensupcom/logs/` (`%LOCALAPPDATA%`
on Windows), keeping the last five runs, and a crash leaves a report in
`crashes/` beside it. To send them with a bug report:

```bash
opensupcom --collect-logs            # -> ./opensupcom-logs-<date>-<time>.zip
```

The zip holds the logs, crash reports, the engine's settings and a
`system.txt` (build, OS, GPU); FA's `Game.prefs`, which holds your player
name, is left out. `--log <file>` puts a run's log somewhere else.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow, the formatting and
clang-tidy checks, the test layers and the engine's less obvious conventions.

## License

This project is licensed under the [MIT License](LICENSE). It holds no game data, and its code is written anew: see [docs/provenance-and-compatibility.md](docs/provenance-and-compatibility.md) for what may enter the repository, and for what the engine promises to run and interoperate with.
