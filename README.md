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

The engine can run a **fully playable single-player skirmish match** — from the front-end lobby through game setup, real-time gameplay, to score screen and return-to-lobby. AI armies load with FA's brain system, autonomously build bases (power generators, factories, engineers), produce armies, and attack. The complete game loop is functional: lobby map/faction/AI personality selection → army creation → AI initialization (OnCreateAI, ExecutePlan, 37+ active AI threads) → autonomous base building and army production → real-time combat with economy, construction, air/naval/land movement → game-over detection → score screen → return to lobby for another match. AI personality types (adaptive, rush, turtle, tech, random) and difficulty settings (cheat multipliers for build rate and income) are configurable via CLI.

Over 163 milestones have been completed across the simulation, renderer, UI, and game infrastructure. The engine bootstraps the full FA Lua import chain (Unit.lua, AIBrain, platoons, categories, economy) and executes autonomous AI behavior with base building, factory production, engineer assist, threat evaluation, platoon formation, and combat engagement. Over 111 former moho stubs have been converted to real implementations. A Vulkan renderer provides real-time visualization with textured 3D SCM unit meshes, GPU blend-weight skeletal animation, team colors, normal mapping, Blinn-Phong specular, PCF soft shadows (4096²), 9-stratum terrain blending with per-stratum normal maps, 5,000+ map props, 2,000+ terrain decals, animated water, fog of war, atmospheric fog, LOD mesh switching, bloom post-processing, particle system with emitter blueprints, frustum culling, and death animations. The full MAUI UI framework provides 13 control types with reactive LazyVar layout, a complete Vulkan 2D rendering pipeline, and GLFW input dispatch. A complete game HUD delivers a playable RTS experience with minimap, strategic zoom, economy bars, selection info, command visualization, control groups, camera bookmarks, and full overlay rendering.

**What works today (Milestones 1-163):**

- Lua 5.0 VM (LuaPlus fork) with full VFS and blueprint loading (8,260 blueprints)
- Session lifecycle: map loading, army creation, brain initialization
- Entity system: units, props, projectiles, shields with full Lua lifecycle callbacks
- Economy: per-unit production/consumption, army aggregation, storage, stalling with efficiency scaling
- Construction: building placement, build progress, factory production, engineer assist
- Orders: Move, Stop, Attack, Guard, Patrol, Reclaim, Repair, Capture, Build, Enhance, Dive with command queues
- Combat: weapons, auto-targeting, projectile flight, damage pipeline, unit death
- AI: brain threads, categories, spatial queries, threat evaluation, platoon management, HuntAI attack loops, autonomous base building (FindPlaceToBuild, BuildStructure, factory production), personality selection (adaptive/rush/turtle/tech/random), cheat difficulty (build rate and income multipliers via buff system)
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
- Moho stub conversions: 111 stubs converted to real implementations across 5 milestones (M35 + M49-M51 + M65), covering brain events/utility, weapon fire/control/targeting, projectile collision/child spawning, platoon formation/targeting, damage/kill flags, command caps, movement/fuel/speed multipliers, navigator, elevation, rotation, visibility, scale, mesh override, collision shapes, attachment system, and more
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
- Mass stub conversions I-III: 84 stubs (weapon Change*, movement mults, fuel, projectile guidance, damage/kill flags, command caps, build restrictions, elevation, weapon targeting/priorities, brain events/utility, platoon formation/targeting)
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
- **MAUI UI framework (M71-M76):**
  - UIControl C++ base class with parent/children tree, moho.control_methods (24 real methods: Destroy/GetParent/SetParent/Show/Hide/SetAlpha/GetRootFrame/HitTest/etc.), Group and Frame classes, InternalCreateGroup/InternalCreateFrame factories, LazyVar reactive layout properties (Left/Top/Width/Height/Depth), GLFW input event dispatch, per-frame OnFrame callbacks, UIControlRegistry
  - Bitmap control: 21 real bitmap_methods (SetNewTexture single/multi DDS, SetSolidColor, SetColorMask, SetUV, SetTiled, UseAlphaHitTest, BitmapWidth/Height via DDS header parsing, animation Play/Stop/Loop/Frame/Pattern, ShareTextures), InternalCreateBitmap factory, GetTextureDimensions global
  - Text control: 9 real text_methods (SetNewFont, SetNewColor, SetText/GetText, SetDropShadow, SetNewClipToWidth, SetCenteredVertically/Horizontally, GetStringAdvance), InternalCreateText factory, 4 font metric LazyVars (FontAscent/FontDescent/FontExternalLeading/TextAdvance)
  - Edit control: 31 real edit_methods (SetText/GetText/ClearText, 5 color pairs, caret, highlight, max chars, font, enable/disable, focus)
  - ItemList control: 20 real item_list_methods (Add/Delete/Modify/GetItem, selection, font, 6-color SetNewColors, scroll, show flags)
  - Scrollbar control: 4 real scrollbar_methods (SetScrollable with Lua ref tracking, SetNewTextures, DoScrollLines/Pages)
  - Border control: 2 real border_methods (SetNewTextures 6-texture ninepatch, SetSolidColor), BorderWidth/BorderHeight LazyVars
  - Dragger control: standalone mouse drag tracking (Destroy, PostDragger global for active dragger dispatch)
  - Cursor control: 5 real cursor_methods (SetNewTexture, SetDefaultTexture, ResetToDefault, Show, Hide)
  - Movie control: 7 stub movie_methods (InternalSet, IsLoaded, Play, Stop, Loop, GetFrameRate, GetNumFrames), MovieWidth/MovieHeight LazyVars
  - Histogram control: 3 stub histogram_methods (SetData, SetXIncrement, SetYIncrement — deprecated)
  - WorldMesh control: 16 real world_mesh_methods (Destroy, SetMesh, SetStance, SetHidden/IsHidden, SetColor, SetScale, 5x parameter setters, 5x GetInterpolated* returning stub tables)
  - Game UI bootstrap: GetFrame(0) root frame with LazyVars, GetNumRootFrames, SetCursor, frame_methods (GetTopmostDepth/GetTargetHead/SetTargetHead), UIWorldView (19 methods with __init constructor, Project, CameraReset, etc.), WldUIProvider_methods, discovery_service_methods (GetGameCount/Reset/Destroy + InternalCreateDiscoveryService), lobby_methods (18 methods + InternalCreateLobby)
- **UI Vulkan 2D rendering pipeline (M77-M78):**
  - UIRenderer: control tree walk, instanced textured quads (48B UIInstance), pixel-to-NDC conversion, alpha blend no depth test, depth-sorted back-to-front rendering, consecutive-texture batching
  - Font rendering: stb_truetype GPU atlas (R8→RGBA swizzle), per-glyph quad emission with drop shadow, centering, clip-to-width, real TrueType metrics replacing all heuristic values, FontCache + FontMetricsProvider
- **UI rendering features (M79-M89):**
  - Scissor/clip rectangles: Vulkan dynamic scissor, clip rect stack intersection during tree walk, controls outside clip rect skipped
  - Border 9-patch rendering: 8 quads (4 corners + 4 edges), BorderWidth/BorderHeight from DDS dimensions
  - Edit control visuals: caret rendering (blinking vertical line), selection highlight, per-glyph cursor positioning
  - ItemList rendering: multi-row text items, scroll offset, selection highlight row, scissor clipping
  - Scrollbar rendering: thumb position/size from scroll state, 3-piece texture (background, thumb-top, thumb-mid)
  - Bitmap animation rendering: accumulator-based frame timing, forward/backward/pingpong patterns, auto-stop/loop
  - Tiled bitmap rendering: UV repeat override, sampler REPEAT wrap mode
  - Input event dispatch: GLFW keyboard/mouse callbacks → event buffer → hit-test tree walk → HandleEvent Lua callbacks, keyboard focus routing, mouse enter/exit tracking
  - OnFrame update loop: NeedsFrameUpdate controls get OnFrame(self, dt) per rendered frame
  - Cursor rendering: custom cursor texture at mouse-hotspot position, topmost depth layer
  - Drag visual feedback: active dragger intercepts mouse events (OnMove/OnRelease/OnCancel), ESC cancellation
- **VFX & Command Systems (M90-M96):**
  - VFX/emitter system: 14 Create* globals (CreateEmitterAtEntity/AtBone/Attached, CreateBeamEmitter/AttachedBeam, CreateLightParticle, CreateDecal/Splat), IEffect tracked objects with parameter overrides, IEffectRegistry with auto-expiry GC
  - CollisionBeam entity system: beam enable/disable, endpoint tracking, Lua lifecycle
  - Decal/Splat system: CreateDecal/CreateSplat Lua globals with tracked IEffect objects
  - Issue commands from Lua: IssueMove/Attack/Guard/Patrol/Reclaim/Repair/Capture/Build/Enhance/Dive/TransportLoad/TransportUnload/Nuke/Tactical, economy event tracking
  - Resource deposits: mass/hydrocarbon deposit entities with Lua queries
  - Interactive game loop: pause/resume, sim speed control (0.5x-10x), title bar stats display
  - Player input command pipeline: left-click unit selection, Shift+click additive selection, drag-box area selection, right-click move/attack commands, minimap click-to-jump
- **Game HUD & Overlays (M97-M112):**
  - Interactive RTS camera: arrow+WASD+mouse-edge scrolling, middle-mouse orbit, scroll-wheel zoom with acceleration, camera speed scales with altitude
  - Game overlays: health bars (green/yellow/red by HP fraction), selection rings (army-colored diamonds), command queue lines (8 command types with distinct colors), build progress bars, waypoint markers
  - Win/lose detection: EndGame/defeat checks, game-over banner overlay (victory/defeat/draw)
  - Minimap: heightmap texture rendering, army-colored unit dots, camera frustum outline, click-to-jump and drag-to-pan
  - Strategic icons: procedural 224x32 icon atlas (7 shapes), zoom-based 3D mesh→2D icon switch at 250u altitude, army-colored
  - Resource economy HUD: mass/energy bars with fill+income indicators, format_number display, DrawGroup-based font switching
  - Selection info panel: single-unit (icon+name+HP bar) and multi-unit (grid of type-grouped icons with counts) display modes
  - Command queue visualization: full command chain lines, 8 command types with distinct colors, entity-targeted projection, waypoint markers
  - Control groups: Ctrl+0-9 assign, 0-9 recall, dead unit pruning on recall
  - Camera bookmarks: Ctrl+Shift+0-9 save, Shift+0-9 recall
  - Beam rendering: construction/reclaim/repair/capture operation beams + CollisionBeam weapon beams (army-colored, AABB quad approximation)
  - Shield bubble rendering: projected 16-segment circle outline, army-colored, HP-based alpha, filled center quad
  - Veterancy indicators: gold chevron squares above health bars (capped at 5)
  - Adjacency lines: orange lines between adjacent selected buildings (dedup by lower-ID draws)
  - Intel range circles: 24-segment projected circles for Radar (teal), Sonar (blue), Omni (yellow), Vision (faint green)
  - Wreckage desaturation: BT.601 luminance-based color reduction for prop wreckage meshes
  - VFX particle rendering: billboard emitter particles at entity+offset positions, beam lines between entities, light particles as larger glowing squares
  - Transport cargo indicators: light blue dots below unit (capped at 8)
  - Silo ammo indicators: nuke (red, left) and tactical (blue, right) dots (capped at 5 each)
- **Visual Polish (M113-M117):**
  - Water rendering: tessellated grid from heightmap, 3 overlapping sine wave displacement attenuated near shore, depth-based coloring (shallow teal→deep blue), Fresnel specular glint, animated shore foam, 84-byte push constants (viewProj+time+eye+waterElev)
  - Fog of war rendering: R8_UNORM GPU texture from VisibilityGrid (Vision=255, Radar/Omni=200, EverSeen=100, Unexplored=0), bilinear sampled in terrain shader, brightness multiplier per fog state, per-frame CPU→GPU upload with pipeline barriers
  - Projected selection circles: 12-segment projected circle outlines replacing 4-point diamonds, AABB line quad approximation
  - Death/explosion VFX: SimState death event pipeline (Lua Destroy→SimState→overlay renderer), age-based particle system with expanding orange flash, bright white center, 4 scattered debris particles, 0.6s duration, max 64 concurrent
  - Build preview ghost: semi-transparent placement mesh at mouse cursor, screen→world projection with grid snapping, footprint-aware even/odd alignment, pathfinding grid validity check (green=valid, red=invalid), alpha=0.35, SetBuildGhost/ClearBuildGhost Lua bindings
- **Terrain Polish (M118-M121):**
  - PCF soft shadows: 4x4 kernel (16 samples) replacing single-sample hard shadows across terrain, unit cubes, and skeletal meshes
  - Terrain specular + hemisphere ambient: Blinn-Phong specular (power 24, intensity 0.15), hemisphere ambient lighting (sky blue above, warm brown below) replacing flat 0.3 ambient, eye position in terrain push constants (120B)
  - Shadow map quality: 4096x4096 depth map (up from 2048), smooth edge fade at shadow frustum boundaries via smoothstep
  - Atmospheric distance fog: exponential fog (density 0.0018) fading terrain/units/meshes/water to blue-grey haze at distance, FoW-aware haze darkening, water alpha increases in fog, unit cube push constants expanded (76B) with eye position
- **Advanced Rendering (M122-M134):**
  - Particle system: EmitterBlueprintData parsed from .bp Lua tables (15+ time-parameterized curves), ParticleSystem CPU sim (Euler integration, curve-driven emission), ParticleRenderer with 2 Vulkan pipelines (alpha + additive blend), instanced billboards, depth test on / depth write off
  - Decal normal maps: CPU-baked RG overlay (RGBA8, 1 texel/game unit), BC3 decoder, terrain shader perturbs blended stratum normals
  - Frustum culling: Gribb-Hartmann plane extraction from VP matrix, `is_sphere_visible()` per entity in UnitRenderer/OverlayRenderer/ParticleSystem
  - LOD mesh switching: LODEntry/LODSet structs, load LODs[1]-[4] from blueprints, distance-based mesh selection per entity
  - Death animations: `dying_` state with configurable duration, begin_dying() clears commands and sets do_not_target, tick_dying() countdown to wreckage
  - Bloom post-processing: HDR render split, bright extract threshold, 9-tap separable Gaussian blur at half-res, additive composite, B key toggle
- **Single-Player Skirmish Infrastructure (M135-M152):**
  - WorldView: UIWorldView with camera control, Project() screen↔world coordinate mapping, IssueSim commands from UI
  - Command pipeline: SimCallback bridge between UI Lua VM and sim Lua VM, build placement with footprint snapping and validity checking
  - HUD panels: construction panel, orders panel, unit info panel with real-time stat display, tooltips
  - Game state machine: GameStateManager (INIT→FRONT_END→LOADING→GAME→SCORE) with Lua callback firing on transitions
  - Beat system: per-army economic/military beat callbacks at configurable intervals
  - Score and game-over: EndGame detection, score accumulation, game-over screen with stats
  - Front-end UI: main menu, skirmish lobby with map/faction/AI selection, FrontEndData cross-state key-value store
  - Preferences: GetPreference/SetPreference with nested key lookup, profile system
  - Hotkeys: KeyMapRegistry with stacked key binding tables, GLFW→key name mapping, configurable bindings
  - Chat system: SendSystemMessage, chat callback dispatch
  - End-to-end integration: lobby→loading→gameplay→score→return-to-lobby full cycle
- **Playable Skirmish (M153-M163):**
  - Smoke test harness: method/global interceptors for missing-method detection, panic handler, logged pcall for error recording
  - Sim generation safety: static generation counter, `check_entity()`/`check_brain()` verify `_c_sim_gen` before dereferencing handles — prevents stale pointers across reloads
  - Sim reload: 19-step reload sequence (clear scene, destroy old state, fresh Lua VM, rebind blueprints, new SimState, load scenario, boot sim, rebuild scene)
  - Return-to-lobby: `ReturnToLobby` Lua global, sim teardown, transition to FRONT_END, null sim_state guards throughout render loop
  - Air movement: heading-based flight (atan2 Y-up convention), pitch/bank/airspeed interpolation, altitude over terrain (not water), blueprint fields (MaxAirspeed, TurnSpeed, Elevation)
  - Air combat: weapon layer targeting (AntiAir/DirectFire/AntiNavy bitmasks), bombing runs with ballistic projectile drop, fuel consumption with crash on empty
  - Air crash physics: gravity + spin + forward momentum, splash damage on impact, debris particles
  - Air separation: boids-style repulsion for same-army air units within 8u radius
  - Naval movement: water depth storage, draft-aware passability in PathfindingGrid, naval Y positioning with sub dive depth, amphibious auto layer transitions (Land↔Water)
  - Torpedoes: homing/tracking with stay_underwater clamping
  - Veterancy: XP tracking, 5-level progression, damage-based XP awards, stat bonuses per level (HP regen, max health)
  - AI-vs-AI validation: two AI armies load with FA's adaptive AI brain, OnCreateAI succeeds, ExecutePlan runs, 37+ active threads, 6000+ tick stable game loop
  - Score tracking: real GetArmyStat/SetArmyStat storage, kill/loss/resource accumulation, score screen with meaningful stats
  - Performance: periodic Lua GC (every 50 ticks), pathfinding request throttle (8/tick cap), simultaneous-death Draw handling
- 92 unit tests (1,527 assertions), 70+ integration test flags

**What's not yet implemented:**

- Networking and multiplayer sync
- Remaining moho binding stubs (mostly cosmetic/polish)

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

# Play a skirmish against a rush AI with cheat difficulty
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" \
  --ai-personality rushcheat
```

AI personality options: `adaptive`, `rush`, `turtle`, `tech`, `random` (and cheat variants: `adaptivecheat`, `rushcheat`, `turtlecheat`, `techcheat`, `randomcheat`). Cheat personalities get 2x build rate and 2x income multipliers.

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
  ui/          # UI control system (UIControl base, UIControlRegistry, input dispatch, font metrics)
  renderer/    # Vulkan renderer (terrain mesh, SCM mesh units, water plane, camera, shaders, mesh cache, UI 2D pipeline, font atlas, HUD, minimap, overlays, strategic icons, selection info, input handler)
  main.cpp     # Entry point, CLI flags, test harnesses
third_party/
  lua-5.0/     # Vendored Lua 5.0 (LuaPlus fork)
  stb/         # stb_truetype for font rendering
tests/         # Catch2 unit tests
```

## License

This project is licensed under the [MIT License](LICENSE).
