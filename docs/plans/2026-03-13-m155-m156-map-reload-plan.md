# M155-M156: Map Reload & Game Loop Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable full lobby→game→score→lobby cycle with actual map reload — `LaunchSinglePlayerSession` loads the selected map, and the score screen returns to lobby for another game.

**Architecture:** SimState and sim Lua state are heap-allocated (`unique_ptr`) so they can be destroyed/recreated on map reload. A `Renderer::clear_scene()` method tears down GPU resources. Entity handle safety uses a global sim generation counter checked in moho binding helpers. BlueprintStore persists across reloads with a `rebind()` method to refresh its Lua refs.

**Tech Stack:** C++17, Vulkan, Lua 5.0, CMake + vcpkg

---

## File Structure

**Modify:**
- `src/renderer/renderer.hpp` — add `clear_scene()`, `caches_initialized_` flag
- `src/renderer/renderer.cpp` — implement `clear_scene()`, guard cache init in `build_scene()`
- `src/sim/sim_state.hpp` — add static sim generation counter
- `src/sim/sim_state.cpp` — implement generation counter (increment in constructor)
- `src/blueprints/blueprint_store.hpp` — add `rebind(lua_State*)`
- `src/blueprints/blueprint_store.cpp` — implement `rebind()`
- `src/lua/moho_bindings.cpp` — add `_c_sim_gen` storage in entity tables, generation check in `check_entity()`
- `src/lua/sim_bindings.cpp` — store `_c_sim_gen` in `create_unit_core()`
- `src/main.cpp` — heap-allocate sim objects, implement reload handler, score→lobby transition
- `tests/test_smoke_test.cpp` — generation counter tests

---

## Chunk 1: Scene Teardown & Entity Safety

### Task 1: Renderer::clear_scene()

**Files:**
- Modify: `src/renderer/renderer.hpp:292-301`
- Modify: `src/renderer/renderer.cpp:1360+`

- [ ] **Step 1: Add clear_scene() declaration and caches_initialized_ flag to renderer.hpp**

In `src/renderer/renderer.hpp`, add `clear_scene()` to the public section (after `build_scene`) and `caches_initialized_` to the private section:

```cpp
// In public section, after build_scene declaration (line ~52):
/// Tear down scene-specific GPU resources for map reload.
void clear_scene();
```

```cpp
// In private section, after initialized_ (line ~301):
bool caches_initialized_ = false;
```

- [ ] **Step 2: Implement clear_scene() in renderer.cpp**

Add before `build_scene()` (around line 1358):

```cpp
void Renderer::clear_scene() {
    vkDeviceWaitIdle(device_);

    terrain_mesh_.destroy(device_, allocator_);
    unit_renderer_.destroy(device_, allocator_);
    water_renderer_.destroy(device_, allocator_);
    fog_renderer_.destroy(device_, allocator_);

    if (bone_ds_pool_) {
        vkDestroyDescriptorPool(device_, bone_ds_pool_, nullptr);
        bone_ds_pool_ = VK_NULL_HANDLE;
        for (auto& ds : bone_ds_) ds = VK_NULL_HANDLE;
    }
    if (terrain_tex_ds_pool_) {
        vkDestroyDescriptorPool(device_, terrain_tex_ds_pool_, nullptr);
        terrain_tex_ds_pool_ = VK_NULL_HANDLE;
        terrain_tex_ds_ = VK_NULL_HANDLE;
    }

    stored_decals_.clear();
    decal_groups_.clear();
    particle_system_.clear();
    emitter_bp_cache_.clear();

    terrain_map_width_ = 0;
    terrain_map_height_ = 0;
    std::fill(std::begin(terrain_strata_scales_),
              std::end(terrain_strata_scales_), 0.0f);
}
```

- [ ] **Step 3: Guard cache init in build_scene() to be idempotent**

In `build_scene()` (line ~1374), wrap the cache init block:

```cpp
// Replace:
if (vfs && sim.blueprint_store()) {
    mesh_cache_.init(...);
    texture_cache_.init(...);
    font_cache_.init(...);
    unit_renderer_.preload_meshes(sim, mesh_cache_, L);
}

// With:
if (vfs && sim.blueprint_store()) {
    if (!caches_initialized_) {
        mesh_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                         vfs, sim.blueprint_store());
        texture_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                            texture_ds_layout_, texture_sampler_, vfs);
        font_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                         texture_ds_layout_, texture_sampler_, vfs);
        caches_initialized_ = true;
    }
    unit_renderer_.preload_meshes(sim, mesh_cache_, L);
}
```

- [ ] **Step 4: Build to verify compilation**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add src/renderer/renderer.hpp src/renderer/renderer.cpp
git commit -m "M155a: Add Renderer::clear_scene() for map reload GPU teardown"
```

---

### Task 2: Sim Generation Counter

**Files:**
- Modify: `src/sim/sim_state.hpp:85-87`
- Modify: `src/sim/sim_state.cpp`
- Test: `tests/test_smoke_test.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_smoke_test.cpp`, add:

```cpp
TEST_CASE("SimState generation increments on construction", "[m155]") {
    // Each SimState construction should increment the global generation
    u32 gen_before = osc::sim::SimState::sim_generation();

    // We can't easily construct a SimState in tests (needs lua_State),
    // but we can test the increment function directly
    osc::sim::SimState::increment_sim_generation();
    REQUIRE(osc::sim::SimState::sim_generation() == gen_before + 1);
}
```

- [ ] **Step 2: Add generation counter to sim_state.hpp**

In `src/sim/sim_state.hpp`, add to the `SimState` public section (after `SECONDS_PER_TICK`, line ~160):

```cpp
/// Global sim generation — incremented each time a SimState is constructed.
/// Used by entity handle safety to detect stale references across reloads.
static u32 sim_generation() { return s_sim_generation_; }
static void increment_sim_generation() { ++s_sim_generation_; }
```

Add to private section (after `has_playable_rect_`, line ~250):

```cpp
static u32 s_sim_generation_;
```

- [ ] **Step 3: Define static member and increment in constructor**

In `src/sim/sim_state.cpp`, add at file scope (after includes):

```cpp
u32 SimState::s_sim_generation_ = 0;
```

In the `SimState` constructor body, add as the first line:

```cpp
++s_sim_generation_;
```

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[m155]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/sim/sim_state.hpp src/sim/sim_state.cpp tests/test_smoke_test.cpp
git commit -m "M155b: Add global sim generation counter for entity handle safety"
```

---

### Task 3: Entity Generation Check in moho_bindings

**Files:**
- Modify: `src/lua/moho_bindings.cpp:65-114` (check_entity, check_unit, check_brain)
- Modify: `src/lua/sim_bindings.cpp` (create_unit_core)

This task makes entity handle dereferences safe across sim reloads. When an entity Lua table is created, we store the current sim generation as `_c_sim_gen`. When `check_entity()` is called, we verify the generation matches before dereferencing the `_c_object` pointer.

- [ ] **Step 1: Store `_c_sim_gen` in create_unit_core**

In `src/lua/sim_bindings.cpp`, find `create_unit_core()`. After the line that stores `_c_object` in the entity Lua table (looks like `lua_pushstring(L, "_c_object"); lua_pushlightuserdata(L, ...); lua_rawset(L, ...);`), add:

```cpp
// Store sim generation for stale-handle detection across reloads
lua_pushstring(L, "_c_sim_gen");
lua_pushnumber(L, static_cast<f64>(SimState::sim_generation()));
lua_rawset(L, unit_tbl);
```

Also add `#include "sim/sim_state.hpp"` if not already present (it likely is since sim_bindings already uses SimState).

- [ ] **Step 2: Add generation check to check_entity()**

In `src/lua/moho_bindings.cpp`, modify `check_entity()` (around line 65). The current implementation extracts `_c_object` as a raw `Entity*`. Add a generation check:

```cpp
// Current code (approximately):
static Entity* check_entity(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* e = static_cast<Entity*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return e;
}

// Replace with:
static Entity* check_entity(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;

    // Check sim generation — stale handles from a previous SimState return nullptr
    lua_pushstring(L, "_c_sim_gen");
    lua_rawget(L, idx);
    if (lua_isnumber(L, -1)) {
        u32 stored_gen = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (stored_gen != sim::SimState::sim_generation()) {
            return nullptr;  // Stale handle from old SimState
        }
    } else {
        lua_pop(L, 1);
        // No generation stored — legacy table, allow through
    }

    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* e = static_cast<Entity*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return e;
}
```

Add `#include "sim/sim_state.hpp"` to the includes in `moho_bindings.cpp` if not present.

- [ ] **Step 3: Also store `_c_sim_gen` in ArmyBrain Lua tables**

In `src/lua/session_manager.cpp`, in `create_army_brain()` (around line 290), after storing `_c_object`, add:

```cpp
lua_pushstring(L, "_c_sim_gen");
lua_pushnumber(L, static_cast<f64>(sim::SimState::sim_generation()));
lua_rawset(L, brain_tbl);
```

Add `#include "sim/sim_state.hpp"` if not already present.

- [ ] **Step 4: Add generation check to check_brain()**

In `moho_bindings.cpp`, modify `check_brain()` similarly to `check_entity()` — add the same generation check before extracting `_c_object`.

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All existing tests pass

- [ ] **Step 6: Commit**

```bash
git add src/lua/moho_bindings.cpp src/lua/sim_bindings.cpp src/lua/session_manager.cpp
git commit -m "M155c: Add sim generation check to entity/brain handle extraction"
```

---

### Task 4: BlueprintStore::rebind()

**Files:**
- Modify: `src/blueprints/blueprint_store.hpp:86-90`
- Modify: `src/blueprints/blueprint_store.cpp`

- [ ] **Step 1: Add rebind() declaration**

In `src/blueprints/blueprint_store.hpp`, add to the public section (after `expose_to_lua`, line ~85):

```cpp
/// Re-bind to a new Lua state after sim reload.
/// Clears all lua_ref values (old state is dead — do NOT unref).
/// Caller must re-run load_blueprints() afterward to repopulate refs.
void rebind(lua_State* new_L);
```

- [ ] **Step 2: Implement rebind()**

In `src/blueprints/blueprint_store.cpp`, add:

```cpp
void BlueprintStore::rebind(lua_State* new_L) {
    L_ = new_L;
    // Clear all lua refs — the old Lua state is destroyed, so we must NOT
    // call luaL_unref. Just reset to -1 (LUA_NOREF equivalent).
    for (auto& [id, entry] : blueprints_) {
        entry.lua_ref = -1;
    }
    spdlog::info("BlueprintStore rebound to new Lua state ({} blueprints, refs cleared)",
                 blueprints_.size());
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add src/blueprints/blueprint_store.hpp src/blueprints/blueprint_store.cpp
git commit -m "M155d: Add BlueprintStore::rebind() for sim Lua state reload"
```

---

## Chunk 2: Reload Wiring

### Task 5: Heap-Allocate Sim Objects in main.cpp

**Files:**
- Modify: `src/main.cpp`

This task converts `sim_lua_state` and `sim_state` from stack variables to `unique_ptr` so they can be destroyed and recreated during map reload.

- [ ] **Step 1: Change sim_lua_state to unique_ptr**

Near line 500 (after the init config setup), change:

```cpp
// Old:
osc::lua::LuaState sim_lua_state;

// New:
auto sim_lua_state = std::make_unique<osc::lua::LuaState>();
```

Add `#include <memory>` to the includes if not present (it likely is from other unique_ptrs).

- [ ] **Step 2: Update all sim_lua_state references**

Replace `sim_lua_state.` with `sim_lua_state->` and `sim_lua_state.raw()` with `sim_lua_state->raw()` throughout main.cpp. Key locations:
- `loader.execute_init(sim_lua_state, ...)` → `loader.execute_init(*sim_lua_state, ...)`
- `BlueprintStore store(sim_lua_state.raw())` → `BlueprintStore store(sim_lua_state->raw())`
- `loader.load_blueprints(sim_lua_state, ...)` → `loader.load_blueprints(*sim_lua_state, ...)`
- `sim_lua_state.raw()` → `sim_lua_state->raw()` (all occurrences)
- `sim_loader.boot_sim(sim_lua_state, ...)` → `sim_loader.boot_sim(*sim_lua_state, ...)`
- `session_mgr.start_session(sim_lua_state, ...)` → `session_mgr.start_session(*sim_lua_state, ...)`
- `sim_lua_state.do_string(...)` → `sim_lua_state->do_string(...)`

Note: Functions that take `LuaState&` need `*sim_lua_state`, not `sim_lua_state->`.

- [ ] **Step 3: Change sim_state to unique_ptr**

Near line 562, change:

```cpp
// Old:
osc::sim::SimState sim_state(sim_lua_state.raw(), &store);

// New:
auto sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
```

- [ ] **Step 4: Update all sim_state references**

Replace `sim_state.` with `sim_state->` throughout main.cpp. Key patterns:
- `sim_state.set_sound_manager(...)` → `sim_state->set_sound_manager(...)`
- `sim_state.set_bone_cache(...)` → `sim_state->set_bone_cache(...)`
- `sim_state.set_anim_cache(...)` → `sim_state->set_anim_cache(...)`
- `sim_state.add_army(...)` → `sim_state->add_army(...)`
- `sim_state.army_count()` → `sim_state->army_count()`
- `sim_state.tick()` → `sim_state->tick()`
- `sim_state.player_result()` → `sim_state->player_result()`
- `renderer.build_scene(sim_state, ...)` → `renderer.build_scene(*sim_state, ...)`
- `renderer.render(sim_state, ...)` → `renderer.render(*sim_state, ...)`
- All other `sim_state.` → `sim_state->`

Note: Functions that take `SimState&` or `const SimState&` need `*sim_state`.

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass — this is a purely mechanical refactor

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "M155e: Heap-allocate sim_lua_state and sim_state for reload support"
```

---

### Task 6: Implement Reload Sequence in Launch Handler

**Files:**
- Modify: `src/main.cpp:1183-1222`

This is the core reload task. Replace the TODO stub in the `__osc_launch_requested` handler with a full sim teardown + rebuild sequence.

- [ ] **Step 1: Replace the launch handler body**

In `src/main.cpp`, find the `__osc_launch_requested` handler block (around line 1183-1222). Replace the body inside `if (!launch_scenario.empty()) { ... }` with:

```cpp
spdlog::info("Launch requested: {}", launch_scenario);

// === RELOAD SEQUENCE ===

// 1. GPU fence — ensure no in-flight work
renderer.clear_scene();

// 2. Destroy old SimState and sim Lua state
sim_state.reset();
sim_lua_state.reset();

// 3. Create fresh sim Lua state
sim_lua_state = std::make_unique<osc::lua::LuaState>();
sim_lua_state->set_vfs(&vfs);
sim_lua_state->set_blueprint_store(&store);

// 4. Run init sequence on new sim state (polyfills, config, class, import)
auto reinit_result = loader.execute_init(*sim_lua_state, config, vfs);
if (!reinit_result) {
    spdlog::error("Reload init failed: {}", reinit_result.error().message);
    // Fall through — game state will be broken but won't crash
}

// 5. Rebind BlueprintStore to new Lua state and reload blueprints
store.rebind(sim_lua_state->raw());
auto rebp_result = loader.load_blueprints(*sim_lua_state, vfs, store);
if (!rebp_result) {
    spdlog::error("Reload blueprint load failed: {}", rebp_result.error().message);
}

// 6. Create fresh SimState
sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);

// 7. Audio, bone cache, anim cache
{
    auto new_sound = std::make_unique<osc::audio::SoundManager>(
        config.fa_path / "sounds");
    lua_State* sL = sim_lua_state->raw();
    lua_pushstring(sL, "osc_sound_manager");
    lua_pushlightuserdata(sL, new_sound.get());
    lua_rawset(sL, LUA_REGISTRYINDEX);
    sim_state->set_sound_manager(std::move(new_sound));
}
sim_state->set_bone_cache(
    std::make_unique<osc::sim::BoneCache>(&vfs, &store));
sim_state->set_anim_cache(
    std::make_unique<osc::sim::AnimCache>(&vfs));

// 8. Load scenario from selected map
osc::lua::ScenarioLoader new_scenario_loader;
auto new_meta_result = new_scenario_loader.load_scenario(
    *sim_lua_state, vfs, launch_scenario, *sim_state);
if (!new_meta_result) {
    spdlog::error("Reload scenario failed: {}",
                  new_meta_result.error().message);
} else {
    scenario_meta = new_meta_result.value();
    for (const auto& army : scenario_meta.armies) {
        sim_state->add_army(army, army);
    }
}
if (sim_state->army_count() == 0) {
    sim_state->add_army("ARMY_1", "Player");
}

// 9. Register GiveResources SimCallback on new state
{
    lua_State* sL = sim_lua_state->raw();
    lua_pushstring(sL, "SimCallbacks");
    lua_rawget(sL, LUA_GLOBALSINDEX);
    if (!lua_istable(sL, -1)) {
        lua_pop(sL, 1);
        lua_newtable(sL);
        lua_pushstring(sL, "SimCallbacks");
        lua_pushvalue(sL, -2);
        lua_rawset(sL, LUA_GLOBALSINDEX);
    }
    sim_lua_state->do_string(R"(
        local sc = rawget(_G, 'SimCallbacks')
        sc.GiveResources = function(args)
            if not args or not args.From or not args.To then return end
            LOG('GiveResources: army ' .. tostring(args.From) .. ' -> army ' .. tostring(args.To) ..
                ' mass=' .. tostring(args.Mass or 0) .. ' energy=' .. tostring(args.Energy or 0))
        end
    )");
    lua_settop(sL, 0);
}

// 10. Store game state manager in new sim registry
{
    lua_State* sL = sim_lua_state->raw();
    lua_pushstring(sL, "__osc_game_state_mgr");
    lua_pushlightuserdata(sL, &game_state_mgr);
    lua_rawset(sL, LUA_REGISTRYINDEX);
}

// 11. Boot sim (registers moho/sim bindings, runs simInit.lua)
osc::lua::SimLoader new_sim_loader;
auto new_sim_result = new_sim_loader.boot_sim(
    *sim_lua_state, vfs, *sim_state);
if (!new_sim_result) {
    spdlog::error("Reload sim boot failed: {}",
                  new_sim_result.error().message);
}

// 12. Start session
{
    osc::lua::SessionManager new_session_mgr;
    new_session_mgr.set_ai_armies({1}); // ARMY_2 is AI
    auto sess_result = new_session_mgr.start_session(
        *sim_lua_state, vfs, *sim_state, scenario_meta);
    if (!sess_result) {
        spdlog::warn("Reload session start failed: {}",
                     sess_result.error().message);
    }
}

// 13. Update UI state's sim_state registry pointer to new SimState
{
    lua_pushstring(uiL, "osc_sim_state");
    lua_pushlightuserdata(uiL, sim_state.get());
    lua_rawset(uiL, LUA_REGISTRYINDEX);
}

// 14. Rebuild renderer scene
renderer.build_scene(*sim_state, &vfs, uiL);

// 15. Reset camera to map center (spherical coords: target + distance)
if (sim_state->terrain()) {
    f32 cx = sim_state->terrain()->map_width() * 0.5f;
    f32 cz = sim_state->terrain()->map_height() * 0.5f;
    renderer.camera().set_target(cx, cz);
    renderer.camera().set_distance(300.0f);
}

// 16. Update UI state registry pointers
lua_pushstring(uiL, "__osc_scenario_path");
lua_pushstring(uiL, launch_scenario.c_str());
lua_rawset(uiL, LUA_REGISTRYINDEX);

lua_pushstring(uiL, "__osc_hover_entity_id");
lua_pushnumber(uiL, 0);
lua_rawset(uiL, LUA_REGISTRYINDEX);

lua_pushstring(uiL, "__osc_focus_army");
lua_pushnumber(uiL, 0);
lua_rawset(uiL, LUA_REGISTRYINDEX);

// 17. Clear selection
input_handler.set_selected({});
prev_selection.clear();

// 18. Reset game state
game_state_mgr.set_game_over(false);
game_state_mgr.set_paused(false, uiL);
sim_accumulator = 0.0;

// 19. Transition to GAME
game_state_mgr.transition_to(
    osc::GameState::LOADING, uiL);
game_state_mgr.transition_to(
    osc::GameState::GAME, uiL);
osc::core::call_start_game_ui(uiL);

spdlog::info("=== Map reload complete ===");
```

- [ ] **Step 2: Verify InputHandler and Camera APIs**

Verify that `InputHandler::set_selected()` accepts an empty set (it does — `set_selected(const std::unordered_set<u32>& sel)`).

Verify that `Camera::set_target(f32 x, f32 z)` and `Camera::set_distance(f32 d)` exist (they do — defined in `src/renderer/camera.hpp`). The camera uses spherical coordinates (target + distance + yaw + pitch), so we reset via `set_target()` + `set_distance()`, NOT by setting an eye position directly.

- [ ] **Step 4: Build to verify**

Run: `cmake --build build --config Debug 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "M155f: Implement full reload sequence in launch handler"
```

---

## Chunk 3: Score Screen & Restart

### Task 7: Score Screen Transition

**Files:**
- Modify: `src/main.cpp:969-996` (game-over detection block)

Currently, game-over detection auto-pauses and calls `NoteGameOver`. We need to also transition the GameStateManager to SCORE state.

- [ ] **Step 1: Add SCORE transition to game-over block**

In `src/main.cpp`, find the game-over detection block (around line 969-996). After `game_state_mgr.set_game_over(true);`, add:

```cpp
game_state_mgr.transition_to(osc::GameState::SCORE, uiL);
```

This fires any UI callbacks registered for the SCORE state transition.

- [ ] **Step 2: Build to verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "M156a: Transition to SCORE state on game over"
```

---

### Task 8: Return-to-Lobby Handler

**Files:**
- Modify: `src/main.cpp` (render loop, near the launch handler)

Add a new registry flag check (`__osc_return_to_lobby`) that tears down the current game and returns to the front-end.

- [ ] **Step 1: Add return-to-lobby handler in the render loop**

In `src/main.cpp`, after the `__osc_launch_requested` handler block (around line 1222), add a new block:

```cpp
// Check for return-to-lobby request (M156b — score screen "Continue")
{
    lua_State* uiL = ui_lua_state->raw();
    lua_pushstring(uiL, "__osc_return_to_lobby");
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    if (lua_toboolean(uiL, -1)) {
        lua_pop(uiL, 1);

        // Clear the flag
        lua_pushstring(uiL, "__osc_return_to_lobby");
        lua_pushnil(uiL);
        lua_rawset(uiL, LUA_REGISTRYINDEX);

        spdlog::info("Returning to lobby...");

        // Tear down game state
        renderer.clear_scene();
        sim_state.reset();
        sim_lua_state.reset();

        // Reset game state
        game_state_mgr.set_game_over(false);
        game_state_mgr.set_paused(false, uiL);
        sim_accumulator = 0.0;
        input_handler.set_selected({});
        prev_selection.clear();

        // Clear hover
        lua_pushstring(uiL, "__osc_hover_entity_id");
        lua_pushnumber(uiL, 0);
        lua_rawset(uiL, LUA_REGISTRYINDEX);

        // Transition to FRONT_END
        game_state_mgr.transition_to(
            osc::GameState::FRONT_END, uiL);

        // Re-show lobby UI
        osc::core::call_lua_global(uiL, "CreateUI");

        spdlog::info("=== Returned to lobby ===");
    } else {
        lua_pop(uiL, 1);
    }
}
```

- [ ] **Step 2: Guard sim ticking against null sim_state**

In the fixed-timestep sim ticking block (around line 999-1006), add a null check:

```cpp
// Old:
if (!game_state_mgr.paused()) {
    sim_accumulator += dt * game_state_mgr.speed();
    while (sim_accumulator >= osc::sim::SimState::SECONDS_PER_TICK) {
        sim_state->tick();
        sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
    }
}

// New:
if (!game_state_mgr.paused() && sim_state) {
    sim_accumulator += dt * game_state_mgr.speed();
    while (sim_accumulator >= osc::sim::SimState::SECONDS_PER_TICK) {
        sim_state->tick();
        sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
    }
}
```

- [ ] **Step 3: Guard all sim_state accesses in the render loop**

Find ALL places in the render loop that access `sim_state` (both `sim_state->` and `*sim_state` dereferences) and add null checks. Key locations that MUST be guarded:

1. `sim_state->player_result()` (game-over check, ~line 970) — wrap: `if (sim_state) { ... }`
2. `renderer.render(*sim_state, ...)` — wrap: `if (sim_state) { renderer.render(...); } else { /* skip sim render in lobby */ }`
3. `input_handler.update(renderer, *sim_state, dt)` — wrap: `if (sim_state) { input_handler.update(renderer, *sim_state, dt); }`
4. SimCallback processing — wrap: `if (sim_state && sim_lua_state) { ... }`
5. Any other `sim_state->` or `*sim_state` — search the render loop for ALL occurrences

When `sim_state` is null (FRONT_END/lobby state), the render loop should only process UI events and render the UI layer. Skip all sim-dependent rendering and input processing.

- [ ] **Step 4: Register ReturnToLobby as a UI Lua function**

The score screen's "Continue" button needs a way to set `__osc_return_to_lobby`. Add a Lua-callable function. In the UI bindings section of main.cpp (where `__osc_exit_requested` is set up), or as a global on the UI state:

```cpp
// After setting up the UI Lua state, register a global function:
{
    lua_State* uiL = ui_lua_state->raw();
    lua_pushstring(uiL, "ReturnToLobby");
    lua_pushcfunction(uiL, [](lua_State* L) -> int {
        lua_pushstring(L, "__osc_return_to_lobby");
        lua_pushboolean(L, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);
        return 0;
    });
    lua_rawset(uiL, LUA_GLOBALSINDEX);
}
```

Place this near the other UI Lua registry setup (around line 840-890).

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "M156b: Add return-to-lobby handler and null guards for sim state"
```

---

### Task 9: Build and Smoke Test Verification

**Files:** None (verification only)

- [ ] **Step 1: Full build**

Run: `cmake --build build --config Debug`
Expected: Build succeeds with no errors

- [ ] **Step 2: Run unit tests**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass

- [ ] **Step 3: Run smoke test**

Run: `cmd //c 'build\Debug\opensupcom.exe --smoke-test --map /maps/SCMP_009/SCMP_009_scenario.lua'`
Expected: 0 unique errors (same as M154 baseline)

- [ ] **Step 4: Manual verification (if windowed mode available)**

Launch normally with `--map` flag. Verify:
1. Game loads and runs
2. Game-over triggers SCORE state
3. Full render loop continues without crashes

- [ ] **Step 5: Commit any fixups**

If smoke test reveals issues, fix them and commit.

```bash
git add -A
git commit -m "M155-M156: Fix smoke test issues from reload implementation"
```

---

## Notes

### What's NOT in scope for M155-M156
- **Reading FrontEndData for army config** — currently hardcoded `set_ai_armies({1})`. A proper implementation would read the lobby's army setup from FrontEndData. Deferred to M162+ integration.
- **Sim Lua state re-registration of UI-accessible sim functions** — The UI state doesn't call sim bindings directly; it uses SimCallbacks. The sim callback queue persists (stack variable). The new sim state gets SimCallbacks registered in step 9.
- **Texture cache eviction** — Old map textures stay in cache. Not a problem for M155-M156 scope (memory is cheap, and the cache would only grow by one map's worth of textures per reload).

### Key invariants
- `sim_state` and `sim_lua_state` may be null when in FRONT_END state (lobby with no game loaded)
- All render-loop code that accesses `sim_state` must null-check
- BlueprintStore persists across reloads — its Lua refs are refreshed via `rebind()` + `load_blueprints()`
- Entity handles are safe across reloads via generation counter check in `check_entity()`/`check_brain()`
- The sim generation counter is global (per-SimState-construction), NOT per-entity-slot. It protects against cross-reload stale references only. Within-session use-after-free (entity destroyed mid-game while UI holds reference) is a separate concern not addressed here — those cases already return nullptr when the entity is removed from the registry.
- Weapon, Navigator, Platoon `_c_object` handles exist only in the sim Lua VM, which is destroyed on reload. No cross-VM safety needed for those types.
