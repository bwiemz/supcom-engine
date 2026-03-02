# OpenSupCom

An open-source reimplementation of the Moho engine that powers *Supreme Commander: Forged Alliance*.

## Project Goal

OpenSupCom aims to be a **1:1 compatible reimplementation** of the Supreme Commander: Forged Alliance engine (the "Moho" engine). The goal is to run unmodified FA game data and Lua scripts — including the full [FAForever](https://www.faforever.com/) mod stack — against a clean, modern C++ codebase.

This is **not** a compatibility layer or wrapper around the original binary. It is a ground-up rewrite that reimplements the engine's internal systems: the Lua VM bridge, simulation tick loop, entity lifecycle, economy, construction, combat, and AI — so that the existing FA Lua gameplay code runs without modification.

### Why?

The original Moho engine is closed-source, 32-bit, single-threaded, and increasingly difficult to maintain. A clean reimplementation opens the door to:

- **64-bit and cross-platform support** (Windows first, Linux/macOS later)
- **Modern C++ performance** (C++17, no legacy COM/DirectX constraints in the sim)
- **Debuggability** (full source, structured logging, deterministic replay)
- **Community extension** (open codebase for FAForever and modders)

### Current Status

The engine can bootstrap a full FA session on Seton's Clutch (8-player map), spawn all 8 ACUs, run the complete FA Lua import chain (Unit.lua, AIBrain, platoons, categories, economy), and execute autonomous AI behavior: base building, factory production, engineer assist, threat evaluation, platoon formation, and combat engagement with pathfinding, weapons fire, enhancements, shields, transports, fog of war with terrain LOS, economy stalling, radar jamming, and real bone-based manipulators. A Vulkan renderer provides real-time visualization of the terrain, units, and water.

**What works today (Milestones 1-40):**

- Lua 5.0 VM (LuaPlus fork) with full VFS and blueprint loading (8,260 blueprints)
- Session lifecycle: map loading, army creation, brain initialization
- Entity system: units, props, projectiles, shields with full Lua lifecycle callbacks
- Economy: per-unit production/consumption, army aggregation, storage, stalling with efficiency scaling
- Construction: building placement, build progress, factory production, engineer assist
- Orders: Move, Stop, Attack, Guard, Patrol, Reclaim, Repair, Capture, Build, Enhance, Dive with command queues
- Combat: weapons, auto-targeting, projectile flight, damage pipeline, unit death
- AI: brain threads, categories, spatial queries, threat evaluation, platoon management, HuntAI attack loops
- Pathfinding: A* with octile heuristic, path smoothing, dynamic building obstacles, terrain height following, real CanPathTo/CanPathToCell queries, GetThreatBetweenPositions for path danger evaluation
- Structure upgrades: T1->T2 structure upgrade via build system
- Capture: engineer captures enemy units, army transfer
- Toggle system: script bits (shield/weapon/intel/stealth/cloak toggles), dive command, layer changes
- Enhancement system: ACU/SACU self-upgrades (AdvancedEngineering etc.), OnWorkBegin/OnWorkEnd Lua callbacks
- Intel system: per-unit radar/sonar/omni/vision state tracking, enable/disable/radius queries
- Shield system: personal shields with health, regen, energy drain, toggle on/off
- Transport system: air transport load/unload, cargo tracking, attach/detach lifecycle, capacity limits
- Fog of war: per-army visibility grid, Vision/Radar/Sonar/Omni paint, alliance sharing, OnIntelChange callbacks, terrain LOS occlusion (Bresenham height ray), real blip methods with dead-reckoning
- Radar jamming: RadarStealth/SonarStealth filtering, IsKnownFake (Omni reveals jammers), IsMaybeDead (no current intel), dead-reckoning position freeze for out-of-sight entities
- Moho stub conversions: 14 stubs converted to real implementations (ArmyIsCivilian, EntityCategoryCount, CanBuild, ShieldIsOn, CreateProjectile, projectile physics, etc.)
- Audio: XWB/XSB bank parsers, miniaudio backend, PlaySound/SetAmbientSound real implementations, 3D spatial audio
- Vulkan renderer: terrain heightmap mesh, instanced unit cubes with army colors, water plane, RTS camera (WASD/scroll/orbit)
- Bone system: SCM v5 mesh parser, per-blueprint bone cache, bone position/direction queries, ShowBone/HideBone, muzzle bone weapon fire
- Manipulators: 4 real types (Rotate, Anim, Slide, Aim) with per-tick simulation, WaitFor coroutine synchronization, 28 moho method implementations, shortest-arc rotation
- 22 unit tests, 28 integration test flags (`--ai-test`, `--combat-test`, `--fow-test`, `--bone-test`, `--manip-test`, `--canpath-test`, etc.)

**What's not yet implemented:**

- Networking and multiplayer sync
- Full UI and input handling
- Many moho binding stubs (90+ unit methods, entity methods, etc.)
- Full mesh rendering (units currently shown as colored cubes)

## Prerequisites

- **Windows 10/11** (primary platform; Linux/macOS not yet tested)
- **Visual Studio 2022** (v17+) with C++ desktop workload
- **CMake 3.21+**
- **vcpkg** (with `VCPKG_ROOT` environment variable set)
- **Vulkan SDK** (1.0+ for rendering; renderer falls back to headless if unavailable)
- **Supreme Commander: Forged Alliance** (Steam or retail installation)
- **FAForever client** (provides patched game data at `C:/ProgramData/FAForever`)

## Building

```bash
# Clone the repository
git clone https://github.com/bwiemz/supcom-engine.git
cd supcom-engine

# Configure (vcpkg dependencies are installed automatically)
cmake --preset default

# Build
cmake --build build --config Debug
```

This produces:
- `build/Debug/opensupcom.exe` — the engine executable
- `build/tests/Debug/osc_tests.exe` — unit test runner

### Dependencies (managed by vcpkg)

| Package | Purpose |
|---------|---------|
| zlib | Compression (VFS archive reading) |
| minizip | .scd/.nx2 archive extraction |
| spdlog | Structured logging |
| fmt | String formatting |
| catch2 | Unit test framework |
| miniaudio | Audio playback (XWB bank streaming, 3D spatial) |
| glfw3 | Window management (Vulkan surface creation) |
| vulkan-headers | Vulkan API headers |
| vulkan-loader | Vulkan runtime loader |
| vulkan-memory-allocator | GPU memory management (VMA) |
| vk-bootstrap | Vulkan instance/device/swapchain setup |
| shaderc | Runtime GLSL to SPIR-V shader compilation |

A vendored **Lua 5.0** (LuaPlus fork with targeted bug fixes) is included in `third_party/lua-5.0/`.

## Running

The engine requires FA game data to run. It auto-detects the Steam installation path and FAForever data directory.

### Windowed Mode (Renderer)

When launched without `--ticks` or test flags, the engine opens a Vulkan window with real-time rendering:

```bash
# Open a window showing Seton's Clutch with terrain, units, and water
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua"
```

Camera controls:
- **WASD** — Pan camera (speed scales with zoom distance)
- **Mouse scroll** — Zoom in/out
- **Middle mouse drag** — Orbit camera
- **ESC** — Close window

The simulation runs at 10 Hz (fixed timestep) decoupled from the render framerate.

### Headless Mode

Adding `--ticks N` runs the simulation headlessly for N ticks with no window:

```bash
# Run 100 sim ticks on Seton's Clutch (headless)
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --ticks 100

# Run the AI test (ARMY_2 builds a base autonomously, 1200 ticks)
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --ticks 1200 --ai-test

# Run the combat test (AI produces assault bots and attacks, 2000 ticks)
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --ticks 2000 --combat-test

# Run unit tests
./build/tests/Debug/osc_tests.exe
```

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

## Project Structure

```
src/
  core/        # Fundamental types (Vector3, numeric aliases)
  vfs/         # Virtual filesystem (.scd/.nx2 archive mounting)
  map/         # Map loading (.scmap terrain, scenario files, A* pathfinding, visibility grid)
  sim/         # Simulation (Entity, Unit, Projectile, Shield, Platoon, ArmyBrain, economy, intel, bones, manipulators)
  lua/         # Lua<->C++ bridge (moho bindings, sim bindings, session management)
  blueprints/  # Blueprint loading and registry
  audio/       # Audio system (miniaudio, XWB/XSB bank parsers, sound manager)
  renderer/    # Vulkan renderer (terrain mesh, unit cubes, water plane, camera, shaders)
  main.cpp     # Entry point, CLI flags, test harnesses
third_party/
  lua-5.0/     # Vendored Lua 5.0 (LuaPlus fork)
tests/         # Catch2 unit tests
```

## License

Not yet determined. If you're interested in contributing or have licensing questions, please open an issue.
