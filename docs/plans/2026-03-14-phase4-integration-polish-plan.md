# Phase 4: Integration & Polish — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full skirmish games run reliably start-to-finish with meaningful score screen stats, no crashes, and acceptable performance.

**Architecture:** Add army stat storage to ArmyBrain, hook stat accumulation into existing entity lifecycle events (death, construction, economy), add periodic Lua GC to sim tick, add pathfinding request throttling, fix simultaneous-death game-over edge case, and create an automated stress test. All changes are additive — no existing behavior changes.

**Tech Stack:** C++17, Lua 5.0, Catch2

---

## Chunk 1: Score System & Game-Over Reliability

### Task 1: Army Stat Storage on ArmyBrain

Add a `stats_` map to `ArmyBrain` so `GetArmyStat`/`SetArmyStat`/`GetBlueprintStat` return real data instead of hardcoded zeros.

**Files:**
- Modify: `src/sim/army_brain.hpp`
- Modify: `src/sim/army_brain.cpp`
- Modify: `src/lua/moho_bindings.cpp`
- Test: `tests/unit_tests.cpp`

- [ ] **Step 1: Add stat storage to ArmyBrain**

In `src/sim/army_brain.hpp`, add public methods and private member:

```cpp
// --- Army stats (score tracking) ---
void set_stat(const std::string& key, f64 value) { stats_[key] = value; }
f64 get_stat(const std::string& key, f64 default_val = 0.0) const {
    auto it = stats_.find(key);
    return it != stats_.end() ? it->second : default_val;
}
void add_stat(const std::string& key, f64 delta) { stats_[key] += delta; }
```

Add to private section:
```cpp
std::unordered_map<std::string, f64> stats_;
```

- [ ] **Step 2: Implement real GetArmyStat**

In `src/lua/moho_bindings.cpp`, replace `brain_GetArmyStat` (line ~4687):

```cpp
static int brain_GetArmyStat(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) {
        lua_newtable(L);
        lua_pushstring(L, "Value"); lua_pushnumber(L, 0); lua_rawset(L, -3);
        return 1;
    }
    const char* stat_name = luaL_checkstring(L, 2);
    f64 def_val = 0;
    if (lua_isnumber(L, 3)) {
        def_val = lua_tonumber(L, 3);
    }
    f64 val = brain->get_stat(stat_name, def_val);
    lua_newtable(L);
    lua_pushstring(L, "Value"); lua_pushnumber(L, val); lua_rawset(L, -3);
    return 1;
}
```

- [ ] **Step 3: Implement real SetArmyStat**

Replace `brain_SetArmyStat` (line ~4700):

```cpp
static int brain_SetArmyStat(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) return 0;
    const char* stat_name = luaL_checkstring(L, 2);
    f64 value = luaL_checknumber(L, 3);
    brain->set_stat(stat_name, value);
    return 0;
}
```

- [ ] **Step 4: Implement real GetBlueprintStat**

Replace `brain_GetBlueprintStat` (line ~4705). FA's `score.lua` calls `GetBlueprintStat("Units_Killed", cat)` — it expects a plain number (not a table). We accumulate category-qualified stat keys like `"Units_Killed_LAND"`:

```cpp
static int brain_GetBlueprintStat(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain) { lua_pushnumber(L, 0); return 1; }
    const char* stat_name = luaL_checkstring(L, 2);
    // score.lua passes stat name only; category filtering is optional
    f64 val = brain->get_stat(stat_name, 0.0);
    lua_pushnumber(L, val);
    return 1;
}
```

- [ ] **Step 5: Write unit test for army stat storage**

In `tests/unit_tests.cpp`, add:

```cpp
TEST_CASE("ArmyBrain stat storage", "[army][stats]") {
    osc::sim::ArmyBrain brain;
    SECTION("default stat returns default value") {
        REQUIRE(brain.get_stat("Units_Built", 0.0) == 0.0);
        REQUIRE(brain.get_stat("Mass_Collected", 42.0) == 42.0);
    }
    SECTION("set and get stat") {
        brain.set_stat("Units_Built", 5.0);
        REQUIRE(brain.get_stat("Units_Built") == 5.0);
    }
    SECTION("add_stat accumulates") {
        brain.add_stat("Units_Killed", 1.0);
        brain.add_stat("Units_Killed", 1.0);
        brain.add_stat("Units_Killed", 1.0);
        REQUIRE(brain.get_stat("Units_Killed") == 3.0);
    }
}
```

- [ ] **Step 6: Run tests**

Run: `./build/tests/Debug/osc_tests.exe "[army][stats]"`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/sim/army_brain.hpp src/lua/moho_bindings.cpp tests/unit_tests.cpp
git commit -m "Add real army stat storage for GetArmyStat/SetArmyStat/GetBlueprintStat"
```

---

### Task 2: Score Accumulation Hooks

Hook into entity lifecycle events to accumulate stats on the owning ArmyBrain: units built, units killed, units lost, mass collected, energy collected.

**Files:**
- Modify: `src/sim/army_brain.hpp` (add accumulation helpers)
- Modify: `src/sim/army_brain.cpp` (economy accumulation in update_economy)
- Modify: `src/lua/moho_bindings.cpp` (hooks in entity_Destroy and construction complete)

- [ ] **Step 1: Add resource accumulation to update_economy**

In `src/sim/army_brain.cpp`, at the end of `update_economy()` (after storage update), add stat accumulation for total mass/energy collected:

```cpp
// Accumulate total resources collected for score tracking
if (economy_.mass.income > 0) {
    stats_["Economy_TotalProduced_Mass"] += economy_.mass.income * dt;
}
if (economy_.energy.income > 0) {
    stats_["Economy_TotalProduced_Energy"] += economy_.energy.income * dt;
}
```

This requires making `stats_` accessible from `army_brain.cpp`. Since `stats_` is a private member and `update_economy` is a member function, this works directly.

- [ ] **Step 2: Add kill tracking in entity_Destroy**

In `src/lua/moho_bindings.cpp`, inside `entity_Destroy` (line ~732), after the veterancy XP distribution block (line ~844) but before the death event / `mark_destroyed()` section, add stat tracking:

```cpp
// --- Score tracking: record kill/loss stats ---
if (e->is_unit() && sim) {
    i32 victim_army = e->army();
    // Record loss for victim's army
    auto* victim_brain = sim->get_army(victim_army);
    if (victim_brain) {
        victim_brain->add_stat("Units_Lost", 1.0);
    }
    // Credit kill to the army that dealt the most damage.
    // damage_contributions() stores (attacker_entity_id, cumulative_damage) pairs —
    // NOT army indices. Must look up each attacker entity to find its army.
    auto* dying_unit = static_cast<sim::Unit*>(e);
    auto& contribs = dying_unit->damage_contributions();
    if (!contribs.empty()) {
        i32 killer_army = -1;
        f32 max_dmg = 0;
        for (const auto& [attacker_id, dmg] : contribs) {
            if (dmg > max_dmg) {
                auto* attacker = sim->entity_registry().find(attacker_id);
                if (attacker && !attacker->destroyed()) {
                    max_dmg = dmg;
                    killer_army = attacker->army();
                }
            }
        }
        if (killer_army >= 0 && killer_army != victim_army) {
            auto* killer_brain = sim->get_army(killer_army);
            if (killer_brain) {
                killer_brain->add_stat("Units_Killed", 1.0);
            }
        }
    }
}
```

**IMPORTANT:** `damage_contributions()` returns `const std::vector<std::pair<u32, f32>>&` — the first element is the **attacker entity ID** (not an army index). You must look up the entity via `entity_registry().find(attacker_id)` to get its army. See the veterancy XP code at line ~832 for the same pattern.

- [ ] **Step 3: Add units-built tracking in construction complete**

Search for where `OnStopBeingBuilt` fires or where build progress reaches 1.0. The construction completion is handled in `src/sim/unit.cpp` in the `update()` method when `build_progress_ >= 1.0`. Add stat tracking there:

In `src/sim/unit.cpp`, in the build completion branch (where `build_progress_` hits 1.0 and `OnStopBeingBuilt` fires), add:

```cpp
// Track units built for score
// (The builder's army gets credit)
```

Actually, the simplest and most reliable hook is in `entity_Destroy`'s construction-complete path. But a better approach: increment in `brain_BuildUnit` or where the unit transitions from building to built. Since FA's score uses `SetArmyStat` calls from Lua (in `defaultunits.lua:OnStopBeingBuilt`), and those now work with real storage (Task 1), this should happen automatically once Task 1 is done.

If FA's Lua code doesn't call `SetArmyStat` for built units, we can add a C++ fallback. For now, skip this step — Task 1's real `SetArmyStat` enables FA's own Lua score tracking.

- [ ] **Step 4: Run full test suite**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass (existing + new army stat tests)

- [ ] **Step 5: Commit**

```bash
git add src/sim/army_brain.cpp src/lua/moho_bindings.cpp
git commit -m "Add score accumulation hooks: resource tracking in economy, kill/loss in entity_Destroy"
```

---

### Task 3: GetArmyScore Real Data

Update `l_GetArmyScore` to return real stats from army stat storage instead of hardcoded zeros.

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (l_GetArmyScore function, line ~13137)

- [ ] **Step 1: Update GetArmyScore to use real stats**

Replace the `general` subtable section in `l_GetArmyScore` (line ~13151):

```cpp
// general subtable
lua_pushstring(L, "general");
lua_newtable(L);

// Compute score: FA uses (units_built * 2 + units_killed * 3 + mass_collected / 100)
f64 units_built = brain->get_stat("Units_Built", 0.0);
f64 units_killed = brain->get_stat("Units_Killed", 0.0);
f64 mass_total = brain->get_stat("Economy_TotalProduced_Mass", 0.0);
f64 score = units_killed * 3.0 + units_built * 2.0 +
            mass_total / 100.0;
set_num("score", score);

{
    int unit_count = 0;
    auto units = brain->get_units(sim->entity_registry());
    for (auto* e : units) {
        if (e && e->is_unit() && !e->destroyed()) unit_count++;
    }
    set_num("currentunits", unit_count);
}
set_num("currentcap", brain->unit_cap());
lua_rawset(L, -3);
```

- [ ] **Step 2: Add units_killed and units_lost to general subtable**

After the `currentcap` line and before `lua_rawset(L, -3)`, add:

```cpp
set_num("kills", brain->get_stat("Units_Killed", 0.0));
set_num("losses", brain->get_stat("Units_Lost", 0.0));
set_num("built", brain->get_stat("Units_Built", 0.0));
set_num("massTotal", brain->get_stat("Economy_TotalProduced_Mass", 0.0));
set_num("energyTotal", brain->get_stat("Economy_TotalProduced_Energy", 0.0));
```

- [ ] **Step 3: Build and verify no compilation errors**

Run: `cmake --build build --config Debug`
Expected: Compiles clean

- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "GetArmyScore returns real stats: score, kills, losses, resource totals"
```

---

### Task 4: Simultaneous Death → Draw

Fix `player_result()` to handle the case where player and all enemies die on the same tick (both ACUs destroyed simultaneously).

**Files:**
- Modify: `src/sim/sim_state.cpp` (player_result method, line ~581)
- Test: `tests/unit_tests.cpp`

- [ ] **Step 1: Fix player_result for simultaneous defeat**

In `src/sim/sim_state.cpp`, update `player_result()` (line ~581):

```cpp
i32 SimState::player_result() const {
    if (!game_ended_) {
        auto* player = army_at(0);
        bool player_defeated = player && player->is_defeated();

        // Check if all non-civilian enemy armies are defeated
        bool all_enemies_dead = true;
        for (size_t i = 0; i < army_count(); i++) {
            auto* brain = army_at(i);
            if (!brain || brain->is_civilian()) continue;
            if (static_cast<i32>(i) == 0) continue; // skip player
            if (player && player->is_ally(static_cast<i32>(i))) continue;
            if (!brain->is_defeated()) {
                all_enemies_dead = false;
                break;
            }
        }

        // Both player and all enemies defeated same tick → draw
        if (player_defeated && all_enemies_dead && army_count() > 1) return 3;
        // Only player defeated → loss
        if (player_defeated) return 2;
        // Only enemies defeated → win
        if (all_enemies_dead && army_count() > 1) return 1;

        return 0; // in progress
    }

    // Game ended — determine result from brain states
    auto* player = army_at(0);
    if (!player) return 3; // draw
    if (player->state() == BrainState::Victory) return 1;
    if (player->state() == BrainState::Defeat ||
        player->state() == BrainState::Recalled) return 2;
    return 3; // draw
}
```

The key change: check `player_defeated && all_enemies_dead` **before** checking each independently — this produces `Draw` instead of `Defeat`.

- [ ] **Step 2: Write integration test for simultaneous death**

SimState requires a Lua state to construct, so this is an integration-level test. Add to `src/main.cpp` as a `--draw-test` flag:

```cpp
bool draw_test = has_flag(argc, argv, "--draw-test");
```

In the test harness section:

```cpp
if (draw_test && sim_state) {
    spdlog::info("=== DRAW TEST: simultaneous ACU death ===");
    // Set both armies to defeated
    for (size_t i = 0; i < sim_state->army_count(); i++) {
        auto* brain = sim_state->army_at(i);
        if (brain && !brain->is_civilian()) {
            brain->set_state(sim::BrainState::Defeat);
        }
    }
    i32 result = sim_state->player_result();
    if (result == 3) {
        spdlog::info("  PASS — simultaneous defeat returns Draw (3)");
    } else {
        spdlog::error("  FAIL — expected Draw (3), got {}", result);
        return 1;
    }
    return 0;
}
```

This directly tests `player_result()` with both armies defeated.

- [ ] **Step 3: Run tests**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All pass

- [ ] **Step 4: Commit**

```bash
git add src/sim/sim_state.cpp tests/unit_tests.cpp
git commit -m "Handle simultaneous ACU death as Draw instead of Defeat"
```

---

## Chunk 2: Performance & Stress Testing

### Task 5: Lua Garbage Collection Integration

Add periodic Lua GC to prevent unbounded memory growth during long games. Currently there are zero GC calls in the entire codebase.

**Files:**
- Modify: `src/sim/sim_state.cpp` (tick method)

**API Note:** Lua 5.0 does **not** have `lua_gc()`. The correct API is `lua_setgcthreshold(L, 0)` (forces immediate full collection) and `lua_getgccount(L)` (returns KB in use). Both are declared in `third_party/lua-5.0/lua.h:207-208`.

- [ ] **Step 1: Add periodic Lua GC to sim tick**

In `src/sim/sim_state.cpp`, at the end of `SimState::tick()` (after VFX gc, around line 300), add:

```cpp
// Periodic Lua garbage collection to prevent unbounded memory growth.
// Lua 5.0 uses stop-the-world mark-and-sweep GC. Setting threshold to 0
// forces an immediate full collection. Running every 50 ticks (5 seconds
// game time) amortizes GC cost while preventing heap growth.
if (tick_count_ % 50 == 0) {
    PROFILE_ZONE("Sim::lua_gc");
    lua_setgcthreshold(L_, 0);
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build build --config Debug`
Expected: Compiles clean.

- [ ] **Step 3: Run tests to verify no regressions**

Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add src/sim/sim_state.cpp
git commit -m "Add periodic Lua GC every 50 ticks to prevent unbounded memory growth"
```

---

### Task 6: Pathfinding Request Throttle

Cap A* pathfinding requests per tick to prevent frame freezes when AI sends 40-unit platoons to attack simultaneously. Excess requests are deferred to the next tick.

**Files:**
- Modify: `src/map/pathfinder.hpp` (add request counter + throttled flag on PathResult)
- Modify: `src/map/pathfinder.cpp` (check budget in find_path)
- Modify: `src/sim/navigator.cpp` (handle throttled result — don't fall back to straight-line)
- Modify: `src/sim/sim_state.cpp` (reset throttle counter per tick)

- [ ] **Step 1: Add per-tick request counter to Pathfinder**

In `src/map/pathfinder.hpp`, add:

```cpp
/// Per-tick pathfinding budget. Returns false if budget exhausted.
bool can_pathfind() const { return requests_this_tick_ < max_requests_per_tick_; }
void increment_request_count() { ++requests_this_tick_; }
void reset_request_count() { requests_this_tick_ = 0; }

static constexpr i32 MAX_REQUESTS_PER_TICK = 8;
```

And add private members:

```cpp
mutable i32 requests_this_tick_ = 0;
static constexpr i32 max_requests_per_tick_ = MAX_REQUESTS_PER_TICK;
```

- [ ] **Step 2: Add throttled result to PathResult**

In `src/map/pathfinder.hpp`, add a `throttled` field to `PathResult`:

```cpp
struct PathResult {
    bool found = false;
    bool throttled = false;  // true if request was deferred (budget exhausted)
    std::vector<Vector3> waypoints;
};
```

- [ ] **Step 3: Check budget in find_path**

In `src/map/pathfinder.cpp`, at the top of `find_path()`, add:

```cpp
if (!can_pathfind()) {
    PathResult r;
    r.throttled = true;
    return r;  // budget exhausted — caller should retry next tick
}
increment_request_count();
```

All methods on Pathfinder that touch `requests_this_tick_` must be callable on a const object since `pathfinder_` is stored as `const` in SimState. Declare all throttle methods as `const` since the counter is `mutable`:

```cpp
bool can_pathfind() const { return requests_this_tick_ < max_requests_per_tick_; }
void increment_request_count() const { ++requests_this_tick_; }
void reset_request_count() const { requests_this_tick_ = 0; }
```

- [ ] **Step 4: Reset counter per tick**

In `src/sim/sim_state.cpp`, at the top of `SimState::tick()` (after incrementing `tick_count_`), add:

```cpp
if (pathfinder_) {
    pathfinder_->reset_request_count();
}
```

- [ ] **Step 5: Handle throttled result in Navigator**

In `src/sim/navigator.cpp`, in the `set_goal` overload that takes a pathfinder (line ~27-37), change the fallback behavior:

```cpp
auto result = pathfinder->find_path(
    current_pos.x, current_pos.z, pos.x, pos.z, layer, draft, amphibious);

if (result.found && !result.waypoints.empty()) {
    waypoints_ = std::move(result.waypoints);
    spdlog::debug("Navigator: path found with {} waypoints", waypoints_.size());
} else if (result.throttled) {
    // Budget exhausted — do NOT fall back to straight-line (would clip walls).
    // Leave waypoints empty; unit stays idle. Navigator::update() will detect
    // idle status, and the unit's command processing will retry next tick.
    spdlog::debug("Navigator: pathfinding throttled, will retry next tick");
    status_ = Status::Idle;
    return;
} else {
    // Genuinely no path found — fall back to straight line
    waypoints_.push_back(pos);
    spdlog::debug("Navigator: no path found, falling back to straight line");
}

status_ = Status::Moving;
```

**IMPORTANT:** Without this change, throttled units would fall back to straight-line movement and walk through walls. The `throttled` flag distinguishes "no path exists" from "try again later".

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All pass. The cap only affects games with 8+ simultaneous path requests per tick.

- [ ] **Step 7: Commit**

```bash
git add src/map/pathfinder.hpp src/map/pathfinder.cpp src/sim/navigator.cpp src/sim/sim_state.cpp
git commit -m "Throttle pathfinding to 8 requests per tick to prevent frame freezes"
```

---

### Task 7: Stress Test Integration Flag

Add `--stress-test` CLI flag that runs an extended AI-vs-AI game (10,000 ticks = ~17 minutes game time) with assertions for stability.

**Files:**
- Modify: `src/main.cpp` (add --stress-test flag and harness)

- [ ] **Step 1: Add --stress-test flag parsing**

In `src/main.cpp`, add to the flag parsing section (near the other `--` flags):

```cpp
bool stress_test = has_flag(argc, argv, "--stress-test");
```

- [ ] **Step 2: Add stress test harness**

In `src/main.cpp`, after the existing `--ai-skirmish` block, add:

```cpp
if (stress_test && sim_state) {
    spdlog::info("=== STRESS TEST: 10000-tick AI-vs-AI ===");

    i32 peak_entities = 0;
    i32 tick_target = 10000;

    for (i32 t = 0; t < tick_target; t++) {
        sim_state->tick();

        i32 entity_count = 0;
        sim_state->entity_registry().for_each([&](const sim::Entity& e) {
            if (!e.destroyed()) entity_count++;
        });
        if (entity_count > peak_entities) peak_entities = entity_count;

        // Log progress every 1000 ticks
        if ((t + 1) % 1000 == 0) {
            spdlog::info("  tick {}/{} — {} entities (peak {})",
                         t + 1, tick_target, entity_count, peak_entities);
        }

        // Check game-over — continue tracking but note it
        auto result = sim_state->player_result();
        if (result != 0 && !sim_state->game_ended()) {
            const char* result_str = result == 1 ? "VICTORY" :
                                      result == 2 ? "DEFEAT" : "DRAW";
            spdlog::info("  Game over at tick {}: {}", t + 1, result_str);
            sim_state->set_game_ended(true);
        }
    }

    // Report results
    spdlog::info("=== STRESS TEST COMPLETE ===");
    spdlog::info("  Peak entities: {}", peak_entities);

    // Report army stats
    for (size_t i = 0; i < sim_state->army_count(); i++) {
        auto* brain = sim_state->army_at(i);
        if (!brain || brain->is_civilian()) continue;
        spdlog::info("  Army {} ({}): kills={:.0f} losses={:.0f} built={:.0f} mass={:.0f}",
                     i, brain->name(),
                     brain->get_stat("Units_Killed"),
                     brain->get_stat("Units_Lost"),
                     brain->get_stat("Units_Built"),
                     brain->get_stat("Economy_TotalProduced_Mass"));
    }

    spdlog::info("  PASS — no crashes in {} ticks", tick_target);
    return 0;
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build --config Debug`
Expected: Compiles clean

- [ ] **Step 4: Run the stress test**

Run:
```bash
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --stress-test
```
Expected: Completes 10,000 ticks without crashing. Logs entity counts and army stats.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Add --stress-test flag: 10000-tick AI-vs-AI stability validation"
```

---

## Chunk 3: Stubs Triage, Docs & Final Validation

### Task 8: Non-Critical Stubs Triage

Triage remaining stubs per Phase 4 spec. The codebase has only 12 stubs remaining (10 cosmetic, 2 gameplay). Document the decision for each.

**Files:**
- No code changes (all stubs remain as-is — all are cosmetic or low-impact)

- [ ] **Step 1: Triage each stub**

| Stub | Type | Location | Decision | Reason |
|------|------|----------|----------|--------|
| `AddManualScroller` | entity | moho_bindings:1852 | **Keep stub** | UI scrolling hint, cosmetic only |
| `AddPingPongScroller` | entity | moho_bindings:1853 | **Keep stub** | UI scrolling hint, cosmetic only |
| `AddThreadScroller` | entity | moho_bindings:1854 | **Keep stub** | UI scrolling hint, cosmetic only |
| `RemoveScroller` | entity | moho_bindings:1855 | **Keep stub** | UI cleanup, cosmetic only |
| `RequestRefreshUI` | entity | moho_bindings:1856 | **Keep stub** | UI refresh signal, cosmetic only |
| `HasValidTeleportDest` | unit | moho_bindings:3333 | **Keep stub** | Teleportation not used in standard skirmish (Quantum Gateway only) |
| `OccupyGround` | unit | moho_bindings:3437 | **Keep stub** (returns true) | Always succeeds — correct for skirmish where terrain deformation isn't implemented |
| `PlayCommanderWarpInEffect` | unit | moho_bindings:3443 | **Keep stub** | Visual effect only |
| `PlayFxRollOffEnd` | unit | moho_bindings:3450 | **Keep stub** | Factory rally VFX only |
| `SetupBuildBones` | unit | moho_bindings:3451 | **Keep stub** | Animation bone setup, cosmetic only |
| `AddBoundedProp` | prop | moho_bindings:4401 | **Keep stub** | Prop attachment, cosmetic only |
| `EconomyEvent.Destroy` | econ | moho_bindings:8159 | **Keep stub** | Cleanup, GC handles it |

**Additional spec-listed stubs:**
| Stub | Decision | Reason |
|------|----------|--------|
| `FlattenMapRect` | **Skip** | Terrain deformation — not needed for gameplay |
| `DrawCircle/DrawLine/DrawLinePop` | **Skip** | Debug rendering — not present in release builds |

All 12 remaining stubs are safe to leave as-is. None affect skirmish gameplay.

- [ ] **Step 2: Commit triage documentation**

No code changes needed. The triage is documented in this plan.

---

### Task 9: Update README with New Test Flags

Add `--stress-test` and `--draw-test` to the integration test table in README.md.

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add --stress-test to test flag table**

In `README.md`, add to the integration test flags table (after `--ai-skirmish`):

```markdown
| `--stress-test` | Extended AI-vs-AI stress test (10,000 ticks, stability validation) |
| `--draw-test` | Simultaneous ACU death → Draw game-over edge case |
```

- [ ] **Step 2: Update milestone count if needed**

Update the "Over 163 milestones" text if this phase constitutes new milestones.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "Add --stress-test to integration test docs"
```

---

### Task 10: Update Memory Files

Update the project memory to reflect Phase 4 completion.

**Files:**
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/MEMORY.md`
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/milestones-list.md`

- [ ] **Step 1: Add Phase 4 milestones to milestones-list.md**

Add a new section:

```markdown
### Phase 7: Integration & Polish (M164-M165)
- M164: Army stat storage, score accumulation, GetArmyScore real data, simultaneous-death Draw
- M165: Lua GC integration, pathfinding throttle, stress test harness
```

- [ ] **Step 2: Update MEMORY.md with Phase 4 decisions**

Add key decisions to MEMORY.md.

- [ ] **Step 3: Commit memory updates**

```bash
git add ~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/
git commit -m "Update project memory with Phase 4 milestones and decisions"
```

---

## Summary

| Task | What it delivers | Files touched |
|------|-----------------|---------------|
| 1 | Real army stat storage (GetArmyStat/SetArmyStat) | army_brain.hpp, moho_bindings.cpp, tests |
| 2 | Score accumulation (kills, losses, resources) | army_brain.cpp, moho_bindings.cpp |
| 3 | GetArmyScore returns real stats for score screen | moho_bindings.cpp |
| 4 | Simultaneous ACU death → Draw | sim_state.cpp, main.cpp |
| 5 | Periodic Lua GC prevents memory growth | sim_state.cpp |
| 6 | Pathfinding throttle prevents frame freezes | pathfinder.hpp/cpp, navigator.cpp, sim_state.cpp |
| 7 | Stress test: 10,000-tick automated validation | main.cpp |
| 8 | Non-critical stubs triage (all cosmetic — keep as-is) | (documentation only) |
| 9 | README + test flag documentation | README.md |
| 10 | Project memory updates | memory files |

**Final success criteria:** `--stress-test` completes 10,000 ticks without crashes, score screen shows non-zero stats (kills, losses, mass collected), `--draw-test` produces Draw.
