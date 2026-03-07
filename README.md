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

The engine can bootstrap a full FA session on Seton's Clutch (8-player map), spawn all 8 ACUs, run the complete FA Lua import chain (Unit.lua, AIBrain, platoons, categories, economy), and execute autonomous AI behavior: base building, factory production, engineer assist, threat evaluation, platoon formation, and combat engagement with pathfinding, weapons fire, enhancements, shields, transports, fog of war with terrain LOS, economy stalling, radar jamming, real bone-based manipulators, and weapon layer cap targeting. Over 111 former moho stubs have been converted to real implementations across five mass conversion milestones. A Vulkan renderer provides real-time visualization with textured 3D SCM unit meshes with GPU blend-weight skeletal animation (4 bones per vertex), team color rendering via SpecTeam alpha masks, normal mapping with tangent-space DXT5nm textures, Blinn-Phong specular lighting, shadow mapping, terrain heightmap with 9-stratum texture blending and per-stratum normal maps, 5,000+ map props (trees, rocks, debris), 2,000+ terrain decals (roads, craters, dirt patches), projectile meshes with velocity-aligned orientation, and water.

**What works today (Milestones 1-70):**

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
- Moho stub conversions: 111 stubs converted to real implementations across 5 milestones (M35 + M49–M51 + M65), covering brain events/utility, weapon fire/control/targeting, projectile collision/child spawning, platoon formation/targeting, damage/kill flags, command caps, movement/fuel/speed multipliers, navigator, elevation, rotation, visibility, scale, mesh override, collision shapes, attachment system, and more
- Audio: XWB/XSB bank parsers, miniaudio backend, PlaySound/SetAmbientSound real implementations, 3D spatial audio
- Vulkan renderer: terrain heightmap mesh, textured SCM mesh rendering (DDS BC1/BC2/BC3 with mipmaps), team color via SpecTeam alpha mask (set=2 descriptor), water plane, RTS camera (WASD/scroll/orbit)
- Bone system: SCM v5 mesh parser, per-blueprint bone cache, bone position/direction queries, ShowBone/HideBone, muzzle bone weapon fire
- Manipulators: 4 real types (Rotate, Anim, Slide, Aim) with per-tick simulation, WaitFor coroutine synchronization, 28 moho method implementations, shortest-arc rotation
- Armor system: per-unit armor types from blueprints, damage multipliers in all damage paths
- Veterancy: HP regen from blueprint, SetRegenRate/RevertRegenRate moho methods
- Wreckage: prop reclaim values, GetHeading quaternion-to-yaw conversion
- Adjacency bonuses: skirt-based neighbor detection, OnAdjacentTo/OnNotAdjacentTo callbacks
- Stats/telemetry: per-unit SetStat/GetStat/UpdateStat for veterancy and scoring
- Silo ammo: nuke and tactical missile counters (Give/Remove/Get), fire-gate pattern
- Targeting flags: SetDoNotTarget, SetReclaimable, IsValidTarget — guards in weapon auto-targeting and reclaim
- SCM mesh rendering: per-blueprint GPU mesh cache, real 3D unit models with directional lighting, DDS texture rendering (albedo from mesh blueprints), cube fallback for missing meshes
- Weapon layer caps: per-weapon `FireTargetLayerCapsTable` enforcement — weapons only auto-target entities on allowed layers (Land, Water, Sub, Seabed, Air)
- Mass stub conversions I–III: 84 stubs (weapon Change*, movement mults, fuel, projectile guidance, damage/kill flags, command caps, build restrictions, elevation, weapon targeting/priorities, brain events/utility, platoon formation/targeting)
- Mass stub conversion IV: 27 stubs (visibility flags, scale, mesh override, collision shapes, attachment system, ShakeCamera, SetUnSelectable)
- SCA skeletal animation: SCA v5 parser, AnimCache lazy loading, per-unit bone matrices, GPU skinning via SSBO, nlerp quaternion interpolation, SCA-to-SCM bone mapping
- Blend-weight skinning: 4 bone indices + equal weights per vertex (SCM v5 convention), GPU multi-bone matrix blending in mesh and shadow shaders, bone index clamping for safety, 64-byte vertex struct (was 48 with rigid skinning)
- Team color rendering: SpecTeam DDS texture alpha mask for selective army color blending, convention-based texture path derivation, per-group Vulkan descriptor binding (set=2)
- Normal map rendering: tangent-space DXT5nm normal maps (`*_normalsTS.dds`), TBN matrix from SCM tangent vectors, GA-channel decode in fragment shader, flat-normal fallback for unmapped meshes
- Map prop rendering: full .scmap binary prop parsing (5,182 trees/rocks/debris on Seton's Clutch), SCMAP section skipper (water/strata/decals/DDS), euler_to_quat + rot_matrix_to_quat orientation, props rendered as textured 3D SCM meshes alongside units
- Prop scale & distance culling: per-entity non-uniform scale (sx/sy/sz), SCMAP scale applied to props, non-uniform model matrix, MAX_INSTANCES raised to 8192, XZ ground-plane distance culling for props (600 unit radius), camera eye position accessor
- Specular lighting: Blinn-Phong specular highlights, eye position in push constants (84 bytes), world-space fragment position, SpecTeam R channel for specular intensity, shininess=32, white specular on top of diffuse+team color
- Terrain textures: 9-stratum blending from .scmap data, embedded DDS blend maps (2x RGBA = 8 overlay weights), per-stratum UV scaling, 108-byte push constants, TextureCache::get_raw() for embedded DDS
- Terrain normal maps: per-stratum DXT5nm normal maps (bindings 11-19), world-aligned TBN with Gram-Schmidt orthogonalization, blended tangent-space normals, 20-binding descriptor set, Lambertian lighting with perturbed normals
- Terrain decal rendering: .scmap binary decal parsing (2,236 decals on Seton's Clutch), instanced textured quads with per-decal model matrices, alpha blending with depth bias z-fighting prevention, LOD distance culling, pre-sorted texture grouping for allocation-free per-frame draw
- Projectile rendering: weapon ProjectileId parsing, blueprint_id on projectiles in all creation paths (C++ auto-fire + 3 Lua paths), velocity-aligned orientation via euler_to_quat, MeshCache mesh/texture resolution for projectile blueprints, UnitRenderer integration (no new pipeline needed)
- Shadow mapping: 2048x2048 depth-only shadow pass (terrain + meshes + cubes), orthographic light frustum centered on camera, comparison sampler with hardware bilinear PCF, calcShadow() in terrain/mesh/unit fragment shaders, depth bias for acne prevention
- Spatial hash grid: 32-unit cell spatial partitioning for EntityRegistry, auto-notify on set_position, O(K) collect_in_radius/collect_in_rect (was O(N) over 5,000+ entities), retroactive entity insertion, swap-and-pop cell removal
- Unit sound: PlayUnitSound/PlayUnitAmbientSound/StopUnitAmbientSound with Blueprint.Audio lookup
- Medium-priority stubs: SetBoneEnabled per-bone anim disable, AddOnGivenCallback with army transfer
- Low-priority stubs: IEffect/CollisionBeam Destroy/BeenDestroyed state, CreateBuilderArmController
- 22 unit tests, 54 integration test flags (`--blend-test`, `--ai-test`, `--combat-test`, `--fow-test`, `--bone-test`, `--manip-test`, `--anim-test`, `--normal-test`, `--prop-test`, `--scale-test`, `--specular-test`, `--terrain-tex-test`, `--terrain-normal-test`, `--decal-test`, `--projectile-test`, `--shadow-test`, `--massstub4-test`, `--spatial-test`, `--unitsound-test`, `--medstub-test`, `--lowstub-test`, etc.)

**What's not yet implemented:**

- Networking and multiplayer sync
- Full UI and input handling
- Remaining moho binding stubs (~29 renderer/VFX stubs: IEffect, CollisionBeam, emitter system)

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
  renderer/    # Vulkan renderer (terrain mesh, SCM mesh units, water plane, camera, shaders, mesh cache)
  main.cpp     # Entry point, CLI flags, test harnesses
third_party/
  lua-5.0/     # Vendored Lua 5.0 (LuaPlus fork)
tests/         # Catch2 unit tests
```

## License

Not yet determined. If you're interested in contributing or have licensing questions, please open an issue.
