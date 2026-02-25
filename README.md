# OpenSupCom

An open-source reimplementation of the Moho engine that powers *Supreme Commander: Forged Alliance*.

## Project Goal

OpenSupCom aims to be a **1:1 compatible reimplementation** of the Supreme Commander: Forged Alliance engine (the "Moho" engine). The goal is to run unmodified FA game data and Lua scripts — including the full [FAForever](https://www.faforever.com/) mod stack — against a clean, modern C++ codebase.

This is **not** a compatibility layer or wrapper around the original binary. It is a ground-up rewrite that reimplements the engine's internal systems: the Lua VM bridge, simulation tick loop, entity lifecycle, economy, construction, combat, and AI — so that the existing FA Lua gameplay code runs without modification.

### Why?

The original Moho engine is closed-source, 32-bit, single-threaded, and increasingly difficult to maintain. A clean reimplementation opens the door to:

- **64-bit and cross-platform support** (Windows first, Linux/macOS later)
- **Modern C++ performance** (C++20, no legacy COM/DirectX constraints in the sim)
- **Debuggability** (full source, structured logging, deterministic replay)
- **Community extension** (open codebase for FAForever and modders)

### Current Status

The engine can bootstrap a full FA session on Seton's Clutch (8-player map), spawn all 8 ACUs, run the complete FA Lua import chain (Unit.lua, AIBrain, platoons, categories, economy), and execute autonomous AI behavior: base building, factory production, engineer assist, threat evaluation, platoon formation, and combat engagement with unit movement and weapons fire.

**What works today (Milestones 1-22):**

- Lua 5.0 VM (LuaPlus fork) with full VFS and blueprint loading (8,260 blueprints)
- Session lifecycle: map loading, army creation, brain initialization
- Entity system: units, props, projectiles with full Lua lifecycle callbacks
- Economy: per-unit production/consumption, army aggregation, storage
- Construction: building placement, build progress, factory production, engineer assist
- Orders: Move, Stop, Attack, Guard, Patrol, Reclaim, Build with command queues
- Combat: weapons, auto-targeting, projectile flight, damage pipeline, unit death
- AI: brain threads, categories, spatial queries, threat evaluation, platoon management, HuntAI attack loops
- 22 unit tests, 10 integration test flags (`--ai-test`, `--combat-test`, etc.)

**What's not yet implemented:**

- Rendering (simulation-only for now — headless tick execution)
- Pathfinding (units move in straight lines)
- Networking and multiplayer sync
- Audio
- Full UI and input handling
- Many moho binding stubs (99+ unit methods, entity methods, etc.)

## Prerequisites

- **Windows 10/11** (primary platform; Linux/macOS not yet tested)
- **Visual Studio 2022** (v17+) with C++ desktop workload
- **CMake 3.21+**
- **vcpkg** (with `VCPKG_ROOT` environment variable set)
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

A vendored **Lua 5.0** (LuaPlus fork with targeted bug fixes) is included in `third_party/lua-5.0/`.

## Running

The engine requires FA game data to run. It auto-detects the Steam installation path and FAForever data directory.

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
| `--ai-test` | Full AI base building (16 entities) |
| `--reclaim-test` | Prop reclaim system |
| `--platoon-test` | Platoon creation, commands, disbanding |
| `--threat-test` | Threat queries and platoon targeting |
| `--combat-test` | AI army production and HuntAI attack loop |

## Project Structure

```
src/
  core/        # Fundamental types (Vector3, numeric aliases)
  vfs/         # Virtual filesystem (.scd/.nx2 archive mounting)
  map/         # Map loading (.scmap terrain, scenario files)
  sim/         # Simulation (Entity, Unit, Projectile, Platoon, ArmyBrain, economy)
  lua/         # Lua↔C++ bridge (moho bindings, sim bindings, session management)
  blueprints/  # Blueprint loading and registry
  main.cpp     # Entry point, CLI flags, test harnesses
third_party/
  lua-5.0/     # Vendored Lua 5.0 (LuaPlus fork)
tests/         # Catch2 unit tests
```

## License

Not yet determined. If you're interested in contributing or have licensing questions, please open an issue.
