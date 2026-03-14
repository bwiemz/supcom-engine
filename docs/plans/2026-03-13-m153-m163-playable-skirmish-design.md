# Playable Skirmish Roadmap — M153-M163

**Goal:** Close all remaining gaps so a human can play a full single-player skirmish match against AI on any FA map, using all unit layers (land, air, naval), from lobby to game-over and back.

**Approach:** Smoke-test-driven — load FA's real Lua codebase first to discover unknown gaps, then fix known gaps (map reload, air, naval, veterancy) informed by real error data, and validate end-to-end.

**Predecessor:** M135-M152 (single-player skirmish infrastructure)

---

## Current State (Post-M152)

### What Works
- **Sim core:** 152 milestones of sim infrastructure — Lua 5.0 VM, VFS, blueprints, entities, units, economy, AI platoons, pathfinding (land), combat, construction, intel, shields, transports, fog of war, 94+ moho stubs converted to real implementations
- **All 9 manipulator types** are real (Rotate, Anim, Slide, Aim, CollisionDetector, FootPlant, Slaver, Storage, Thrust) — `CreateBuilderArmController` maps to AimManipulator
- **All 16+ order types** implemented (Move, Attack, Guard, Patrol, Build, Reclaim, Repair, Capture, Upgrade, Enhancement, Transport, Nuke, Tactical, Overcharge, Sacrifice, Ferry, Dive)
- **EntityCategory** real on sim state, UI state has dedicated `l_ui_EntityCategoryGetUnitList`
- **Renderer:** Vulkan with terrain, meshes, skeletal animation, shadows, particles, bloom, LOD, water, fog, decals, UI
- **UI framework:** All 13 MAUI control types, full input dispatch, font rendering, scissor clips
- **Skirmish infrastructure:** Dual Lua VM, GameStateManager (5 states), beat system, score/game-over, front-end UI, lobby, preferences, hotkeys, chat, SimCallback bridge
- **EndGame()** is real; game-over detection via `player_result()` works

### Confirmed Gaps
1. **Map reload** — `LaunchSinglePlayerSession` sets registry flags but doesn't destroy/create SimState (TODO at main.cpp:~1203)
2. **Air movement** — Layer field exists, air units skip pathfinding (straight-line), but no altitude, no flight physics, no crash-on-death
3. **Naval movement** — No water-passable pathfinding, no submarine depth, no amphibious layer transitions
4. **Veterancy** — `vet_level_` field exists (0-5) but no XP accumulation, no level-up progression, no stat bonuses
5. **Minor stubs** — `IsGameOver` (always false), `SetPlayableRect` (no-op), `ArmyInitializePrebuiltUnits` (no-op), `AddBuildRestriction`/`RemoveBuildRestriction` (no-op)
6. **Unknown gaps** — FA's real Lua code likely calls functions or expects behaviors we haven't discovered yet

### Not Gaps (Verified Working)
- Manipulators (all 9 types real, not dummy objects)
- EntityCategory (real on sim state; engine_bindings.cpp stubs are intentional during blueprint-load phase)
- EndGame() (real; only IsGameOver() is a stub)

---

## Phase 1: FA Lua Smoke Test & Gap Triage (M153-M154)

### M153 — Instrumented Boot Harness

**Purpose:** Load FA's real Lua codebase and produce a categorized failure report.

**Implementation:**
- Add `--smoke-test` CLI flag that expects FA game files mounted via VFS (.scd base + FAF .nx2 patches)
- Install a custom `lua_atpanic` and pcall error handler on both Lua states that logs failures (function name, error message, file:line) but continues execution instead of aborting
- Intercept missing global access: set a `__index` metamethod on the globals table that logs `"MISSING_GLOBAL: <name>"` and returns nil (instead of silently returning nil)
- Intercept missing method calls on moho objects: add `__index` fallback to cached metatables that logs `"MISSING_METHOD: <type>.<name>"` and returns a no-op function
- Boot sequence:
  1. Mount FA game files via VFS
  2. Load system Lua files (both states)
  3. Load blueprints
  4. Boot sim: `SetupSession` → `BeginSession` with a real scenario (Seton's Clutch — has water)
  5. Boot UI: `CreateUI` → `StartGameUI` → `SetupUI`
  6. Run 100 sim ticks + 100 UI frame dispatches
- Output: structured log file with categories:
  - `MISSING_GLOBAL` — function/table not registered
  - `MISSING_METHOD` — method not on moho metatable
  - `PCALL_ERROR` — Lua runtime error (wrong type, nil access, etc.)
  - `WRONG_RETURN` — function returned unexpected type (detected where possible)
- Deduplicate: group by unique (category, name, first-occurrence file:line), count occurrences

**Exit criteria:** Smoke test runs to completion without hard crash. Output is a prioritized gap list.

### M154 — Quick-Fix Triage Round

**Purpose:** Fix all trivially-solvable issues from the smoke test report.

**Known fixes (implement regardless of smoke test):**
- Wire `IsGameOver()` to `player_result() != 0` (1-line change in sim_bindings.cpp)
- Implement `SetPlayableRect(x0, z0, x1, z1)` — store bounds on SimState, clamp unit positions in Navigator::update()
- Implement `ArmyInitializePrebuiltUnits` — iterate save file's prebuilt unit list, call `create_unit_core` for each
- Implement global `AddBuildRestriction(army, category)` / `RemoveBuildRestriction(army, category)` — per-army restriction set checked during build validation. Note: per-unit `unit:AddBuildRestriction(id)` already exists in moho_bindings.cpp; this is the global per-army form (sim_bindings.cpp stubs at lines 4228-4229)

**Smoke-test-driven fixes:**
- For each `MISSING_GLOBAL`: register as no-op stub if cosmetic, or implement if gameplay-affecting
- For each `MISSING_METHOD`: add to appropriate metatable — return sensible default if non-critical
- For each `PCALL_ERROR`: diagnose root cause — often a missing field or wrong return type upstream
- Prioritize: errors that cascade (one missing function causes 50 downstream errors) get fixed first

**Re-run smoke test after fixes to measure progress.** Target: reduce error count by 80%+.

**Exit criteria:** Sim boots and ticks 100+ ticks without fatal errors. UI boots and renders frames. Remaining errors are categorized as "needs air/naval" or "needs veterancy" or "cosmetic."

---

## Phase 2: Map Reload & Game Loop (M155-M156)

### M155 — SimState Reload

**Purpose:** Complete the lobby→game transition so `LaunchSinglePlayerSession` actually loads the selected map.

**Implementation:**
- In main.cpp's `__osc_launch_requested` handler (currently at ~line 1203):
  1. `vkDeviceWaitIdle()` — ensure no in-flight GPU work
  2. Destroy current SimState (destructor exists, safe — clears Lua refs, unique_ptrs auto-destruct)
  3. Clear renderer scene state: mesh instances, terrain buffers, decals, props, particles, fog of war texture, shadow map contents
  4. Parse new scenario from `__osc_launch_scenario` path using existing ScenarioLoader
  5. Create fresh SimState with new map's terrain, pathfinding grid, armies from scenario
  6. Re-upload terrain heightmap to GPU, reload decals/props from new .scmap
  7. Reset camera to new map center
  8. Read FrontEndData session config for army count, AI settings, player army index
  9. Run `boot_sim()` + `BeginSession()` on fresh sim state
  10. Transition GameStateManager: LOADING → GAME
  11. Call `StartGameUI()` on UI state
- Sim Lua state: destroy and recreate (fresh VM for the new game)
- UI Lua state: keep alive (persistent across games — FA's UI is not recreated per match)
- Re-register moho bindings and sim bindings on the new sim Lua state
- Re-register the SimCallback bridge between UI and new sim state

**Key constraint:** UI state references to the old sim (entity IDs, army brain pointers) become invalid on reload. Clear all UI-side entity caches. The UI state's `osc_sim_state` registry pointer (defined as `REG_SIM_STATE` in lua_state.hpp) must be updated to the new SimState.

**BlueprintStore lifecycle:** Blueprints are game-global, not map-specific — the BlueprintStore must persist across map reloads. Since its Lua registry refs point into the old sim Lua state (which is destroyed), blueprint loading must be re-run on the new sim Lua state to re-populate the registry refs. The BlueprintStore itself (C++ object) stays alive; only its Lua-side refs are refreshed.

### M156 — Score Screen & Restart

**Purpose:** Complete the game-over→score→lobby cycle.

**Implementation:**
- When `player_result() != 0` (victory/defeat/draw):
  - Transition GameStateManager to SCORE
  - Fire `NoteGameOver` Lua callback (already exists) with result string
  - FA's Lua UI handles score screen display
- From SCORE state, support transition back to FRONT_END:
  - FA's score screen Lua UI provides a "Continue" button that fires a SimCallback or calls a moho function to request state transition
  - On transition request: destroy current SimState (same as M155 teardown), clear renderer scene
  - Transition to FRONT_END
  - Call `CreateUI()` to re-show lobby
- Full cycle: FRONT_END → LOADING → GAME → SCORE → FRONT_END → LOADING → GAME ...

**Exit criteria:** Can select a map in lobby, launch a game, play until game-over, see score screen, return to lobby, launch a different map — no crashes, no resource leaks.

---

## Phase 3: Air Movement (M157-M159)

### M157 — Flight Physics Core

**Purpose:** FA-accurate air movement — units fly at blueprint elevation, bank on turns, accelerate/decelerate.

**New fields on Unit:**
- `f32 elevation_` — target altitude above terrain, from blueprint `Physics.Elevation` (typically 15-25 for aircraft)
- `f32 current_altitude_` — actual Y offset above terrain (smoothly interpolates toward elevation_)
- `f32 climb_rate_` — vertical speed limit, from blueprint or default 5.0
- `f32 heading_` — yaw angle in radians (new — Entity/Unit currently has no heading field; orientation is quaternion-only)
- `f32 pitch_` — pitch angle in radians (for dive/climb visual)
- `f32 turn_rate_rad_` — yaw rate in radians/sec, from blueprint `Air.TurnSpeed` (degrees → radians)
- `f32 max_airspeed_` — from blueprint `Air.MaxAirspeed`
- `f32 accel_rate_` — from blueprint `Air.AccelerateRate` (fallback: `MaxAirspeed * 0.5` if absent)
- `f32 current_airspeed_` — smoothly ramps toward max
- `f32 bank_angle_` — roll angle for visual banking on turns (proportional to yaw rate)
- Fuel: use existing `fuel_ratio_` (0.0-1.0) and `fuel_use_time_` fields already on Unit

**Navigator changes for Air layer:**
- Skip A* pathfinding entirely (already the case)
- New air-specific update path in Navigator::update():
  - Turn `heading_` toward target using `turn_rate_rad_`, advance along heading at `current_airspeed_`
  - Set entity orientation quaternion from `euler_to_quat(heading_, pitch_, bank_angle_)`
- Altitude: each tick, interpolate `current_altitude_` toward `terrain_height(pos.x, pos.z) + elevation_` at `climb_rate_`
- Set entity Y position to `terrain_height + current_altitude_` (not snapped to terrain)
- Acceleration: ramp `current_airspeed_` from 0 to `max_airspeed_` using `accel_rate_` (read from blueprint `Air.AccelerateRate`)
- Banking: `bank_angle_ = clamp(yaw_delta * 2.0, -0.5, 0.5)` radians — used by renderer for mesh rotation
- Respect playable area bounds (set by M154's SetPlayableRect)

**Renderer changes:**
- Unit renderer: use entity's actual Y position for air units (already the case if pos.y is set correctly)
- Apply bank_angle_ as roll rotation on mesh model matrix
- Air units cast shadows from their flight altitude (shadow map already handles arbitrary Y)

### M158 — Air Combat & Landing

**Purpose:** Air-to-air engagement, bombing, air staging, fuel.

**Weapon layer targeting:**
- Weapon blueprint has `RangeCategory`: `UWRC_AntiAir` (targets air), `UWRC_DirectFire` (targets ground), `UWRC_AntiNavy` (targets water/sub)
- When selecting targets, filter by layer compatibility: AntiAir weapons only acquire Air-layer targets, ground weapons only acquire Land/Water targets
- AA ground units: weapons with AntiAir range category + unit on Land layer → can target Air-layer units (already works if targeting filter is correct)

**Bombing runs:**
- Bombers have weapons with `NeedToComputeBombDrop = true` in blueprint
- On attack command: fly toward target, when within `BombDropThreshold` distance, fire weapon (creates projectile with gravity)
- Bomb projectile: `ballistic_accel` from blueprint (gravity), no tracking — purely ballistic from release altitude
- After bomb release: continue forward (no sharp turns), circle back for another pass if target survives

**Air staging:**
- Air staging platforms (T2 air staging facility): unit with `TransportClass` that accepts Air-layer units
- `IssueTransportLoad` to staging platform: air unit flies to platform, transitions to Land layer, "docked"
- While docked: refuel (`fuel_ = fuel_use_time_`), repair (if platform has repair capability)
- Auto-launch when fuel full or when given new orders

**Fuel system:**
- Units with `fuel_use_time_ > 0`: decrement `fuel_ratio_` by `dt / fuel_use_time_` each tick while airborne (layer == "Air")
- When `fuel_ratio_ <= 0`: unit auto-returns to nearest air staging or crashes
- Lua bindings: `GetFuelRatio()` already returns `fuel_ratio_` directly; `SetFuelUseTime(t)` updates `fuel_use_time_`

### M159 — Air Death & Crash

**Purpose:** Air units crash on death instead of vanishing.

**Crash sequence:**
1. `begin_dying()` called (existing M133 system) — clears commands, sets do_not_target
2. If unit is Air layer: enter crash state instead of normal death timer
   - Disable control inputs (no more Navigator updates)
   - Apply gravity as acceleration: `crash_velocity_y_ += -9.8 * dt; current_altitude_ += crash_velocity_y_ * dt` (true acceleration, not constant velocity)
   - Apply random torque: `bank_angle_ += random_spread * dt`, `heading_ += spin_rate * dt`
   - Play crash animation if `Display.AnimationDeath` exists in blueprint
3. Each tick: check if `current_altitude_ <= 0` (terrain impact)
4. On terrain impact:
   - Spawn wreckage prop at crash site (existing wreckage system)
   - Deal `CrashDamage` (from blueprint `General.CrashDamage`, default 100) to all units within crash radius (radius = unit footprint * 1.5, matching frustum culling bounding radius)
   - Remove the unit entity
5. Renderer: during crash, mesh renders at actual position with increasing bank/pitch angles

**Exit criteria:** Build T1 air factory, produce interceptors/bombers/transports. They fly at correct altitude, bank on turns, engage targets (air-to-air and bombing), land at staging platforms, use fuel. On death they crash to the ground, dealing impact damage. AI can build and use air units.

---

## Phase 4: Naval Movement (M160-M161)

### M160 — Naval Surface & Submarine Movement

**Purpose:** Ships move on water, submarines dive, torpedoes work.

**Pathfinding changes:**
- The pathfinding grid currently marks cells as passable/impassable for land. Add a parallel water passability check:
  - A cell is water-passable if `terrain_height(x, z) < water_elevation` (terrain is below water surface)
  - A cell is land-passable if `terrain_height(x, z) >= water_elevation` (existing behavior)
- Navigator: when layer == "Water" or "Sub", use water-passable grid instead of land-passable grid
- Store `water_elevation` on SimState (parsed from .scmap, already available for water rendering)

**Surface naval movement:**
- Y position = `water_elevation` (constant, not terrain height) — ships float on the water surface
- Movement uses same 2D pathfinding as land but on the water grid
- Turn rate from blueprint `Physics.TurnRate` (ships turn slowly)
- Speed from blueprint `Physics.MaxSpeed`

**Submarine movement:**
- Submerged Y position = `water_elevation + elevation_` where elevation_ is negative (e.g., -3.0 = 3 units below surface)
- Two layers: "Sub" (submerged) and "Water" (surfaced)
- `IssueDive` (already exists as order type): toggle between Sub and Water layer
- Layer transition: smooth Y interpolation over ~1 second between surface and dive depth
- Fire `OnLayerChange` Lua callback (already wired)
- Visibility: Sub-layer units only visible to sonar (intel system already handles this via layer checks)

**Torpedo weapons:**
- Projectiles with `Physics.TrackTarget = true` and targeting Water/Sub layers
- Move underwater: projectile Y clamped to `<= water_elevation`
- **Implement homing/tracking projectile logic** (does not yet exist — `tracking` and `turn_rate` fields exist on Projectile but are unused in `Projectile::update()`): each tick, compute direction to target, rotate velocity toward target by `turn_rate * dt`, advance at projectile speed. ~50-100 lines of steering code in projectile.cpp. This also enables tracking missiles for air-to-air combat (M158)

**Naval factory placement:**
- Build validation: factory footprint cells must all be water-passable
- Factory Y position = water_elevation (built floating on water)
- **Factory footprint vs produced ships:** Naval factories mark footprint cells as Obstacle (via `mark_obstacle()`), which blocks naval pathfinding. Produced ships must be spawned at the factory's rally point (outside the footprint), not inside it. The existing `IssueFactoryRallyPoint` system handles this — verify that rally points default to a position adjacent to the factory footprint on water-passable cells

### M161 — Amphibious Layer Transitions

**Purpose:** Units that can cross water-land boundaries.

**Pathfinding changes for amphibious/hover:**
- Add "Amphibious" and "Hover" cases to `is_passable_for()` in pathfinding_grid.cpp: return true for both `CellPassability::Passable` (land) and `CellPassability::Water` cells, false for `Obstacle`
- Navigator passes the unit's motion type string to the pathfinder instead of just the layer name
- Note: `create_unit_core` in sim_bindings.cpp (line ~506) currently sets Hover units to "Water" layer — this should be changed to "Land" layer (hover units are always Land-layer, they just can traverse water)

**Amphibious units:**
- Blueprint has `Physics.MotionType = 'RULEUMT_Amphibious'`
- Each tick, check terrain height vs water elevation at unit position:
  - If `terrain_height < water_elevation` and current layer is "Land" → transition to "Water", fire `OnLayerChange`
  - If `terrain_height >= water_elevation` and current layer is "Water" → transition to "Land", fire `OnLayerChange`
- Pathfinding: uses "Amphibious" passability (both land and water cells passable)
- Y position: `max(terrain_height, water_elevation)` (walk on terrain or float on water, whichever is higher)

**Hover units:**
- Blueprint `Physics.MotionType = 'RULEUMT_Hover'`
- Always float at water elevation over water, terrain height over land
- Can cross water-land boundaries without layer change (always "Land" layer)
- Pathfinding: uses "Hover" passability (both land and water cells passable, same as amphibious)

**SurfacingSub units (e.g., Cybran Salem class):**
- Blueprint `Physics.MotionType = 'RULEUMT_SurfacingSub'`
- These are naval surface units that can submerge — NOT seabed walkers
- Behavior: same as submarines (Water/Sub layer toggle via IssueDive), handled by M160
- Note: true seabed walkers (e.g., Cybran Brick) use `RULEUMT_Amphibious` with "Seabed" layer — they walk on terrain_height even under water, visible to sonar only

**Exit criteria:** Build naval factories on water, produce destroyers/subs/frigates. Subs dive and surface. Amphibious units (e.g., Wagner) cross land-water boundaries seamlessly. Torpedoes hit naval targets. Hover units cross water without stopping. AI can build and use naval units.

---

## Phase 5: Veterancy (M162)

### M162 — Veterancy XP & Level Progression

**Purpose:** Full FA veterancy — units earn XP from kills, level up, gain stat bonuses.

**New fields on Unit:**
- `f32 vet_xp_` — accumulated experience points (default 0)
- `std::array<f32, 5> vet_thresholds_` — XP required for levels 1-5, from blueprint `Veteran.Level1` through `Veteran.Level5`
- `vet_level_` already exists (u8, 0-5)

**Damage contribution tracking (new infrastructure):**
- Currently only `last_attacker_id_` (single u32) exists — no per-attacker damage ledger
- Add `std::unordered_map<u32, f32> damage_contributions_` to Unit — maps attacker entity ID → cumulative damage dealt
- Record contributions in the `Damage()` global function (sim_bindings.cpp ~line 1257): `victim->damage_contributions_[instigator_id] += amount`
- On unit death: compute XP value = `Economy.BuildCostMass` from victim's blueprint
- Distribute XP to all contributing units proportional to their damage dealt (FA behavior)
- Each recipient calls `add_xp(amount)` → checks thresholds → levels up if crossed
- Clear `damage_contributions_` on death (after distribution) and on `SetHealth` to full (repair resets ledger)

**Level-up mechanics:**
- When `vet_xp_ >= vet_thresholds_[vet_level_]`:
  - Increment `vet_level_`
  - Apply per-level buffs from blueprint `Buffs.Regen`, `Buffs.MaxHealth`, `Buffs.Damage` (cumulative)
  - Regen buff: add to `regen_rate_` (existing field)
  - MaxHealth buff: multiply `max_health_` by factor, heal the difference
  - Damage buff: store `damage_multiplier_` on Unit, apply in weapon damage calculation
  - Fire `OnVeteran` Lua callback on the unit (FA scripts use this for custom vet behavior)
- Level-up is irreversible (XP only increases)

**Lua bindings (all new — none of these exist yet in moho_bindings.cpp):**
- `unit:GetVeterancyLevel()` → returns vet_level_ (0-5) — add to unit_methods metatable
- `unit:SetVeterancyLevel(level)` → directly set level (used by Lua scripts for special cases)
- `unit:AddXP(amount)` → add XP and check for level-up
- `unit:GetXPValue()` → returns the unit's Economy.BuildCostMass (what it's worth as XP)

**Renderer:**
- Vet chevron overlay (M108) already renders based on `vet_level_` — will show correctly once levels are real

**Exit criteria:** Kill an enemy unit, see XP distributed to your units. Units visibly level up (chevrons appear). Stat bonuses apply (more HP, faster regen, more damage). `OnVeteran` callback fires for Lua scripts.

---

## Phase 6: End-to-End Validation (M163)

### M163 — Full Integration Test

**Purpose:** Validate the complete game loop with real FA assets.

**Test scenario:**
1. Launch engine with FA game files mounted
2. Boot into front-end → navigate to skirmish lobby
3. Select Seton's Clutch (water map — tests land + naval + air)
4. Configure: 1 human player + 1 AI opponent, standard rules
5. Launch game → verify M155 map reload works
6. Play through early game:
   - Build land units → verify land movement/combat
   - Build air factory → verify air units fly at correct altitude, bank, engage
   - Build naval factory on water → verify ships move, subs dive
   - Verify veterancy: kill enemies, observe level-ups
7. Trigger game-over: destroy AI ACU
8. Verify score screen appears (M156)
9. Return to lobby → launch a second game on a different map (e.g., The Pass) to test reload
10. Play briefly → exit

**Automated regression:**
- Re-run M153 smoke test harness against final build
- Target: zero fatal errors, <10 non-fatal warnings (cosmetic stubs only)

**Fix any remaining issues** discovered during this pass as sub-tasks of M163.

**Exit criteria:** A human can play a full skirmish match against AI on a water map, using land/air/naval units, from lobby to victory/defeat, see the score screen, return to lobby, and launch another game without crashes.

---

## Milestone Summary

| Phase | Milestones | Description |
|-------|-----------|-------------|
| 1 | M153-M154 | FA Lua smoke test + quick-fix triage |
| 2 | M155-M156 | Map reload + score screen restart cycle |
| 3 | M157-M159 | FA-accurate air flight model |
| 4 | M160-M161 | Naval surface/sub + amphibious transitions |
| 5 | M162 | Veterancy XP/level progression |
| 6 | M163 | End-to-end validation on real FA map |

**Total: 11 milestones (M153-M163)**

## Key Design Decisions

- **Smoke test first:** Unknown gaps discovered early, informing priority of all subsequent work
- **FA-accurate flight model:** Blueprint-driven elevation, banking turns, accel/decel curves, crash-on-death with impact damage
- **Water pathfinding:** Parallel passability grid (water vs land), not a separate pathfinder — reuse existing A* with different passability check
- **Amphibious as combined grid:** Amphibious/hover units path on union of land+water passable cells
- **Veterancy via damage tracking:** XP distributed proportionally to damage dealt, matching FA's original behavior
- **UI state persists across games:** Only sim Lua state is recreated on map reload; UI state stays alive
- **Crash-on-death for air:** Physical simulation (gravity + torque) during death, terrain impact creates wreckage + area damage

## Dependencies

- FA game files (.scd + FAF .nx2 patches) must be available on disk for smoke test (M153)
- Water elevation already parsed from .scmap and available for water rendering
- Wreckage system (M43), death animation system (M133) used by air crash
- Vet chevron renderer (M108) used by veterancy display
- Damage contribution tracking built as part of M162 (only `last_attacker_id_` exists today)
- Projectile homing/tracking logic built as part of M160 (fields exist but update() is straight-line only)
- BlueprintStore re-population on sim Lua state recreate (M155)
- `is_passable_for()` expanded for Amphibious/Hover motion types (M161)

## Lua 5.0 Implementation Notes

- Globals metatable for smoke test (M153): use `lua_pushvalue(L, LUA_GLOBALSINDEX)` then `lua_setmetatable()` — `LUA_GLOBALSINDEX` is a pseudo-index, not a real table
- All blueprint field reads use `lua_pushstring` + `lua_rawget`, never `lua_getfield` (doesn't exist in 5.0)
- No `lua_objlen` — use `luaL_getn` for array lengths
- Registry key naming: follow existing `osc_*` / `__osc_*` patterns
