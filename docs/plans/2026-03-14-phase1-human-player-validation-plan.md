# Phase 1: Human Player Validation — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A human player can select their ACU, open the construction menu, build structures, queue factory production, issue commands (including shift-queue), fight enemy units, and reach a win/lose screen — all without crashes or showstopper stubs.

**Architecture:** Fix known gaps first (shift-queue input, category filter bindings, score data), then run guided smoke tests through each gameplay step to discover and fix remaining issues. Each task targets one gameplay subsystem.

**Tech Stack:** C++17, Lua 5.0, Catch2 (tests), Vulkan (renderer), GLFW (input)

**Reference spec:** `docs/plans/2026-03-14-playable-skirmish-vs-ai-design.md` — Phase 1

---

## File Structure

| Action | Path | Responsibility |
|--------|------|---------------|
| Modify | `src/renderer/input_handler.cpp` | Shift-queue support in right-click commands |
| Modify | `src/lua/moho_bindings.cpp` | EntityCategoryFilterOut/FilterDown on ui_L, GetUnitCommandDataOfUnit, GetArmyScore real data |
| Modify | `src/sim/sim_state.cpp` | ACU death → army defeat detection |
| Modify | `src/sim/sim_state.hpp` | Expose ACU death check helper |
| Modify | `src/sim/army_brain.hpp` | set_state() method if missing |

---

## Chunk 1: Known Fixes

### Task 1: Shift-Queue Support in Right-Click Commands

The input handler always clears existing commands on right-click. FA uses Shift+right-click to queue additional waypoints without clearing.

**Files:**
- Modify: `src/renderer/input_handler.cpp:100-126` (minimap right-click) and `:193-246` (world right-click)

- [ ] **Step 1: Add shift check to `handle_right_click()`**

In `src/renderer/input_handler.cpp`, inside `handle_right_click()`, add a shift key check before the command loop and pass `!shift` to `push_command`:

```cpp
// Add before line 224 (before "// Issue commands to all selected units")
bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
             renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);

// Change line 240 from:
//     unit->push_command(cmd, true); // clear existing commands
// to:
unit->push_command(cmd, !shift); // shift-click queues without clearing
```

- [ ] **Step 2: Add shift check to minimap right-click**

In the same file, the minimap right-click block (lines 100-124) also hardcodes `true`. Apply the same fix:

```cpp
// Add before line 108 (before "for (u32 uid : selected_)")
bool shift = renderer.is_key_pressed(GLFW_KEY_LEFT_SHIFT) ||
             renderer.is_key_pressed(GLFW_KEY_RIGHT_SHIFT);

// Change line 117 from:
//     unit->push_command(cmd, true);
// to:
unit->push_command(cmd, !shift);
```

- [ ] **Step 3: Build and verify compilation**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds with 0 errors.

- [ ] **Step 4: Commit**

```bash
git add src/renderer/input_handler.cpp
git commit -m "Add shift-queue support to right-click commands

Shift+right-click now queues move/attack commands without clearing
existing orders. Applies to both world and minimap right-clicks.
Matches FA's shift-queue behavior for command waypoint chaining."
```

---

### Task 2: EntityCategoryFilterOut and EntityCategoryFilterDown on ui_L

FA's construction panel Lua code uses `EntityCategoryFilterOut(category, unitTable)` and `EntityCategoryFilterDown(category, unitTable)` to filter unit lists. `FilterOut` and `FilterDown` exist on sim_L (sim_bindings.cpp:2720-2727) but neither exists on ui_L. The construction panel (`construction.lua`) calls `EntityCategoryFilterDown` 14+ times.

**Important naming note:** FA's Lua code calls the "keep matching" filter `EntityCategoryFilterDown`, NOT `EntityCategoryFilterIn`. The sim_L implementation at sim_bindings.cpp:2720 confirms this name.

**Files:**
- Modify: `src/lua/moho_bindings.cpp` — add two new static functions + register them in `register_ui_bindings()`

- [ ] **Step 1: Add `l_ui_EntityCategoryFilterOut` and `l_ui_EntityCategoryFilterDown` to moho_bindings.cpp**

Add after the existing `l_ui_EntityCategoryGetUnitList` function (after line 11891):

```cpp
/// Helper: extract Entity* from a ui_L unit table (has _c_object lightuserdata).
/// Returns nullptr if table is missing or entity is invalid.
static sim::Entity* extract_ui_entity(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return e;
}

/// ui_L category filter helper. If keep_matches is true, keeps units matching
/// the category (FilterDown). If false, keeps non-matching (FilterOut).
static int ui_category_filter(lua_State* L, bool keep_matches) {
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    if (!lua_istable(L, 1) || !lua_istable(L, 2)) return 1;

    for (int i = 1; ; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        int unit_tbl = lua_gettop(L);
        auto* entity = extract_ui_entity(L, unit_tbl);
        bool matches = false;
        if (entity && entity->is_unit() && !entity->destroyed()) {
            auto* unit = static_cast<sim::Unit*>(entity);
            matches = osc::lua::unit_matches_category(L, 1, unit->categories());
        }

        if (matches == keep_matches) {
            lua_pushnumber(L, out_idx++);
            lua_pushvalue(L, unit_tbl);
            lua_rawset(L, result);
        }
        lua_pop(L, 1); // pop unit table
    }
    return 1;
}

/// EntityCategoryFilterDown(category, unitList) — keep matching units (ui_L)
static int l_ui_EntityCategoryFilterDown(lua_State* L) {
    return ui_category_filter(L, true);
}

/// EntityCategoryFilterOut(category, unitList) — keep non-matching units (ui_L)
static int l_ui_EntityCategoryFilterOut(lua_State* L) {
    return ui_category_filter(L, false);
}
```

- [ ] **Step 2: Register the new functions in `register_ui_bindings()`**

Add after line 12969 (after the `GetBlueprintIconPath` registration):

```cpp
    // Category filter globals for ui_L (Phase 1 — construction panel needs these)
    state.register_function("EntityCategoryFilterOut",  l_ui_EntityCategoryFilterOut);
    state.register_function("EntityCategoryFilterDown", l_ui_EntityCategoryFilterDown);
```

- [ ] **Step 3: Add the include for `sim/unit.hpp` if not already present**

Check the includes at the top of `moho_bindings.cpp`. It likely already includes `sim/unit.hpp` — verify and add if missing. The `static_cast<sim::Unit*>` in the filter function requires the full Unit class definition.

- [ ] **Step 4: Build and verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Add EntityCategoryFilterOut/FilterDown to ui_L

FA's construction panel Lua code uses these globals to filter unit
lists by category. FilterOut keeps non-matching units, FilterDown
keeps matching units. Reuses the existing categories_match() helper
from category_utils.cpp. Mirrors the sim_L implementations in
sim_bindings.cpp."
```

---

### Task 3: ACU Death Triggers Army Defeat

Currently `ArmyBrain::is_defeated()` only returns true when the BrainState is `Defeat` or `Recalled`, and that state is only set when ALL units are destroyed. In FA, losing your ACU (the COMMAND category unit) immediately triggers defeat. This is critical for the game-over flow.

**Files:**
- Modify: `src/sim/sim_state.cpp` — check for ACU death each tick
- Modify: `src/sim/sim_state.hpp` — add helper declaration if needed
- Test: `tests/test_phase1.cpp`

- [ ] **Step 1: Add ACU death check in `SimState::tick()`**



In `src/sim/sim_state.cpp`, find the existing tick function. Add the ACU death check **before** the existing all-units-destroyed defeat check (around line 241). This ensures ACU death is detected first:

```cpp
// --- ACU death → army defeat check (add BEFORE existing defeat detection) ---
for (size_t ai = 0; ai < army_count(); ai++) {
    auto* brain = army_at(ai);
    if (!brain || brain->is_defeated() || brain->is_civilian()) continue;

    // Check if this army still has a living COMMAND unit (ACU)
    bool has_acu = false;
    auto units = brain->get_units(entity_registry_);
    for (auto* e : units) {
        if (!e->is_unit() || e->destroyed()) continue;
        auto* unit = static_cast<Unit*>(e);
        // Must check is_dying() — dying units are not yet destroyed but
        // are in their death animation (2s). Without this check, defeat
        // detection would be delayed until the death animation completes.
        if (unit->categories().count("COMMAND") > 0 && !unit->is_dying()) {
            has_acu = true;
            break;
        }
    }

    if (!has_acu && !units.empty()) {
        // ACU is dead/dying but army still has other units — trigger defeat
        spdlog::info("Army {} ACU destroyed — triggering defeat", ai);
        brain->set_state(BrainState::Defeat);
    }
}
```

Important notes:
- `!unit->is_dying()` is critical — without it, defeat detection delays by up to 2s (death animation duration). `Unit::is_dying()` returns true when `begin_dying()` has been called.
- The `!units.empty()` guard prevents triggering defeat at game start before units spawn.
- The `is_civilian()` check skips armies that have no ACU by design.
- Place this BEFORE the existing defeat check at line ~241 so ACU death takes priority.

- [ ] **Step 2: Verify `brain->set_state()` exists**

Check `src/sim/army_brain.hpp` for a `set_state()` method. If it doesn't exist, add:

```cpp
void set_state(BrainState s) { state_ = s; }
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/sim/sim_state.cpp src/sim/army_brain.hpp
git commit -m "Trigger army defeat when ACU (COMMAND unit) is destroyed

In FA, losing your ACU immediately ends the game for that army,
even if other units survive. Previously defeat only triggered when
ALL units were destroyed. Now each tick checks whether each army
still has a COMMAND category unit alive."
```

---

### Task 4: GetArmyScore Returns Real Unit Count

The score screen's `GetArmyScore()` currently returns `0` for `currentunits`. It should return the actual count of living units for the army.

**Files:**
- Modify: `src/lua/moho_bindings.cpp:12464-12465` — fix `currentunits` in `l_GetArmyScore()`

- [ ] **Step 1: Fix `currentunits` in `l_GetArmyScore()`**

In `src/lua/moho_bindings.cpp`, replace the stub value at line 12465:

```cpp
// Change from:
//     set_num("currentunits", 0); // simplified
// To:
{
    auto* sim = get_sim(L);
    int unit_count = 0;
    if (sim) {
        auto units = brain->get_units(sim->entity_registry());
        for (auto* e : units) {
            if (e && e->is_unit() && !e->destroyed()) unit_count++;
        }
    }
    set_num("currentunits", unit_count);
}
```

Note: `get_sim(L)` is already called at line 12448. Reuse the existing `sim` pointer — just move the `set_num` call to after the null check:

```cpp
// Simpler: use the sim pointer already at line 12448
int unit_count = 0;
if (sim) {
    auto units = brain->get_units(sim->entity_registry());
    for (auto* e : units) {
        if (e && e->is_unit() && !e->destroyed()) unit_count++;
    }
}
set_num("currentunits", unit_count);
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Return real unit count in GetArmyScore currentunits

Score screen now shows actual living unit count per army instead
of hardcoded 0. Uses ArmyBrain::get_units() to count non-destroyed
units for the queried army."
```

---

## Chunk 2: Human Player Smoke Test

### Task 5: Run Human Player Smoke Test — Lobby to Game Launch

Launch the engine as a human player (not `--ai-skirmish`) with interceptors active. This validates the lobby → game launch → human army setup path.

**Files:**
- No code changes expected (validation task)
- May require fixes discovered during testing

- [ ] **Step 1: Launch with smoke test + map**

Run: `./build/Debug/osc_engine.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua" --smoke-test 2>&1 | head -100`

This runs with interceptors on both sim_L and ui_L. Review the output for:
- Missing globals (MissingGlobal category)
- Missing methods (MissingMethod category)
- Lua pcall errors (PcallError category)

- [ ] **Step 2: Triage smoke test results**

Classify each issue as:
- **Fix now**: Blocks construction panel, factory production, orders, or game-over flow
- **Defer**: Cosmetic (PlayCommanderWarpInEffect, PlayFxRollOffEnd, SetupBuildBones, DrawCircle/DrawLine), performance-only, or AI-only methods

- [ ] **Step 3: Fix blocking issues**

For each "fix now" issue, implement the minimal fix in the appropriate file:
- Missing ui_L moho methods → add to `moho_bindings.cpp`
- Missing sim globals → add to `sim_bindings.cpp`
- Lua errors → trace the error, fix the root cause

- [ ] **Step 4: Re-run smoke test and verify reduced error count**

Run: `./build/Debug/osc_engine.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua" --smoke-test 2>&1 | grep -c "MISSING\|ERROR"`
Expected: Error count should decrease after fixes.

- [ ] **Step 5: Commit fixes**

```bash
git add -A
git commit -m "Fix smoke test issues blocking human player gameplay

[Describe specific fixes applied based on triage results]"
```

---

### Task 6: Validate Construction Panel Flow

The construction panel must work: select ACU → panel shows buildable blueprints → click blueprint → build ghost appears → click to place → ACU builds structure.

**Files:**
- May modify: `src/lua/moho_bindings.cpp` (if missing bindings discovered)

- [ ] **Step 1: Trace the construction panel Lua flow**

Read the FA Lua files that drive the construction panel to identify which moho methods they call:
- `fa/lua/ui/controls/worldview.lua` — build mode entry
- `fa/lua/ui/game/construction.lua` — panel population
- `fa/lua/ui/game/commandmode.lua` — build placement

List every moho method called and check if it exists in `moho_bindings.cpp`.

- [ ] **Step 2: Verify `EntityCategoryGetUnitList` returns buildable blueprints**

This was implemented in M140a. The construction panel calls this to get blueprint IDs matching categories like `BUILTBYTIER1COMMANDER`. Verify the function works by checking the smoke test output for errors in construction panel Lua files.

- [ ] **Step 3: Verify build ghost → SimCallback → construction flow**

The flow is:
1. UI calls `SetBuildGhost(blueprintId)` → sets ghost on SimState
2. Player clicks → UI calls `IssueBuildMobile(units, position, blueprintId)` → queues SimCallback
3. SimCallback drains in main.cpp → dispatches to sim_L `SimCallbacks.BuildMobile`
4. Sim creates the structure entity and assigns the builder

Check each step has a working implementation. Known implementations:
- `SetBuildGhost` / `ClearBuildGhost`: moho_bindings.cpp (M139)
- `IssueBuildMobile`: moho_bindings.cpp:11730-11767 (M138b)
- SimCallback drain: main.cpp:1035-1115

- [ ] **Step 4: Fix any missing bindings or SimCallback handlers**

If the construction panel calls methods that don't exist, add them. Common missing methods for construction:
- `GetBuildRate()` — unit method, may need implementation
- `GetWorkProgress()` — unit method for build progress display
- `GetFractionComplete()` — unit method for build completion percentage

- [ ] **Step 5: Build, test, commit**

```bash
cmake --build build --config Debug
git add -A
git commit -m "Fix construction panel bindings for human player

[Describe specific fixes]"
```

---

### Task 7: Validate Factory Production Flow

Select a built factory → construction panel shows producible units → click unit blueprint → unit queues → factory builds → unit spawns at rally point.

**Files:**
- May modify: `src/lua/moho_bindings.cpp` (if missing bindings discovered)

- [ ] **Step 1: Trace factory production Lua flow**

The factory production panel uses:
- `SetCurrentFactoryForQueueDisplay(factory)` — moho_bindings.cpp (M140c) ✓
- `PeekCurrentFactoryForQueueDisplay()` — moho_bindings.cpp (M140c) ✓
- `DecreaseBuildCountInQueue(factory, index, count)` — moho_bindings.cpp (M140c) ✓
- `IssueUnitCommand(units, commandType, blueprintId)` — moho_bindings.cpp (M138b) ✓

Verify each exists and handles the factory production case correctly.

- [ ] **Step 2: Verify factory queue display via `GetCommandQueue`**

FA uses `GetCommandQueue()` (already implemented at moho_bindings.cpp:1924-1958) for factory queue display, NOT a separate `GetBuildQueue` method. The factory queue display panel (M140c) reads the command queue to show queued production. Verify:
- `GetCommandQueue()` returns build commands for factory units
- `SetCurrentFactoryForQueueDisplay` correctly stores the factory reference
- `PeekCurrentFactoryForQueueDisplay` retrieves it for UI updates

- [ ] **Step 3: Fix any gaps and commit**

```bash
cmake --build build --config Debug
git add -A
git commit -m "Fix factory production flow for human player

[Describe specific fixes]"
```

---

## Chunk 3: Orders and Game-Over

### Task 8: Validate Orders Panel and Add Missing Command Bindings

Select units → order buttons appear (Move, Attack, Patrol, Guard, Stop) → click order → cursor mode changes → click on map → command dispatches.

**Files:**
- Modify: `src/lua/moho_bindings.cpp` — add `GetUnitCommandDataOfUnit` if missing

- [ ] **Step 1: Verify `GetUnitCommandData` returns correct orders**

`GetUnitCommandData` (moho_bindings.cpp:11537-11620) already returns available orders and toggles. Check the `all_caps[]` array includes all FA order types needed:
- Move, Attack, Guard, Patrol, Stop ✓ (confirmed in exploration)
- RetaliateToggle, Repair, Capture, Reclaim ✓
- Overcharge, Transport, Ferry, Sacrifice, Nuke, Tactical, Teleport, Dive, Pause ✓

- [ ] **Step 2: Add `GetUnitCommandDataOfUnit` if missing**

FA's `commandmode.lua:369` and `upgrade-structure.lua:56` call `GetUnitCommandDataOfUnit(unit)` — a single-unit variant of `GetUnitCommandData`. Check if this is registered on ui_L. If missing, add it:

```cpp
/// GetUnitCommandDataOfUnit(unit) — single-unit variant for command mode
static int l_GetUnitCommandDataOfUnit(lua_State* L) {
    // Wrap the single unit into a 1-element array table and delegate
    // to the existing GetUnitCommandData implementation
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushvalue(L, 1); // copy the unit table
    lua_rawset(L, -3);   // {[1] = unit}

    // Replace arg 1 with the wrapped table
    lua_replace(L, 1);
    return l_GetUnitCommandData(L);
}
```

Register in `register_ui_bindings()` near `GetUnitCommandData`:

```cpp
state.register_function("GetUnitCommandDataOfUnit", l_GetUnitCommandDataOfUnit);
```

- [ ] **Step 3: Verify `GetUnitCommandFromCommandCap` maps caps to command types**

This function (registered at moho_bindings.cpp:12950) maps capability strings like "RULEUCC_Move" to command type strings. Verify it handles all FA command caps.

- [ ] **Step 4: Verify `IssueUnitCommand` dispatches commands via SimCallback**

`IssueUnitCommand` (moho_bindings.cpp, M138b) should push commands through the SimCallback queue. Verify it handles:
- Move commands (RULEUCC_Move)
- Attack commands (RULEUCC_Attack)
- Patrol commands (RULEUCC_Patrol)
- Guard commands (RULEUCC_Guard)
- Stop commands (RULEUCC_Stop)

- [ ] **Step 5: Fix any gaps and commit**

```bash
cmake --build build --config Debug
git add -A
git commit -m "Fix order panel command dispatch for human player

Add GetUnitCommandDataOfUnit binding for command mode. Verify
all order types dispatch correctly via SimCallback."
```

---

### Task 9: Validate Economy Feedback

Verify mass/energy bars update during construction and stalling feedback is visible when over-building. This is spec test sequence item #6.

**Files:**
- May modify: `src/lua/moho_bindings.cpp` (if economy query bindings missing)

- [ ] **Step 1: Verify economy bar data flow**

FA's economy bars (`economy.lua`) read economy data via beat functions that call:
- `GetArmyEconomyIncome(army, "MASS")` / `"ENERGY"` — income rates
- `GetArmyEconomyRequested(army, "MASS")` / `"ENERGY"` — consumption rates
- `GetArmyEconomyStored(army, "MASS")` / `"ENERGY"` — current storage

Check if these are registered on ui_L. They may use `GetArmyScore()` data or separate bindings. If separate bindings are needed, search for `GetArmyEconomy` in moho_bindings.cpp.

- [ ] **Step 2: Verify economy data updates during construction**

When the ACU builds a structure, `consumption_mass` and `consumption_energy` should be non-zero on the builder unit's `UnitEconomy`. The army-level economy aggregation in `SimState::tick()` should reflect this in the `ArmyBrain::economy()` struct.

Verify by running the engine with construction and checking that economy values change in the log output.

- [ ] **Step 3: Verify stalling feedback**

When an army is consuming more resources than it produces (stalling), the economy bars should show this visually. In FA, stalling is indicated by the bar color changing and the income/consumption ratio showing red. This is handled by FA's Lua economy UI code — the engine just needs to provide accurate rate data.

Verify that `economy().mass.income` and `economy().mass.requested` are populated correctly during construction.

- [ ] **Step 4: Fix any gaps and commit**

```bash
cmake --build build --config Debug
git add -A
git commit -m "Fix economy feedback data for human player

[Describe specific fixes if any]"
```

---

### Task 10: Validate Game-Over Flow

Kill AI's ACU → victory screen → score data → return to lobby → relaunch.

**Files:**
- May modify: `src/lua/moho_bindings.cpp`, `src/main.cpp`

- [ ] **Step 1: Verify victory detection triggers**

With Task 3's ACU death → defeat change, killing the AI's ACU should trigger:
1. `SimState::player_result()` returns 1 (victory)
2. main.cpp:991-1021 detects `game_result != 0`
3. `game_state_mgr.set_game_over(true)` + `set_paused(true)`
4. `NoteGameOver()` called in ui_L
5. State transitions to SCORE

- [ ] **Step 2: Verify defeat detection triggers**

If the AI kills the player's ACU:
1. Task 3's ACU check marks player army as Defeat
2. `player_result()` returns 2 (defeat)
3. Same game-over flow triggers

- [ ] **Step 3: Verify return-to-lobby works after score screen**

The return-to-lobby flow (main.cpp `__osc_return_to_lobby` handler) should:
1. Clear renderer scene
2. Destroy sim_state + sim_lua_state
3. Reset game state flags
4. Transition to FRONT_END
5. Call CreateUI to rebuild lobby

This was implemented in M156b and should work. Verify by checking the smoke test output for errors during the SCORE → FRONT_END transition.

- [ ] **Step 4: Verify re-launch after return-to-lobby**

The 19-step reload sequence (M155) should handle:
1. Fresh Lua VM creation
2. Blueprint rebinding
3. New SimState
4. Scenario loading

Check for stale state issues — the `_c_sim_gen` generation counter (M153) should prevent stale entity handle crashes.

- [ ] **Step 5: Fix any gaps and commit**

```bash
cmake --build build --config Debug
git add -A
git commit -m "Fix game-over and return-to-lobby flow

[Describe specific fixes]"
```

---

## Chunk 4: Integration Validation

### Task 11: End-to-End Human Player Test

Run a complete gameplay loop: lobby → game → build → fight → win/lose → score → lobby → relaunch.

**Files:**
- No new code expected (validation + any final fixes)

- [ ] **Step 1: Run full integration test**

Launch the engine normally (not smoke test) with a human player + AI opponent:

```bash
./build/Debug/osc_engine.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua"
```

Walk through the Phase 1 test sequence:
1. Lobby loads, map selectable, human + AI slot visible
2. Game launches, ACU visible on map
3. Select ACU → construction panel populates
4. Click T1 factory blueprint → build ghost appears → place → ACU builds
5. Select factory → production panel shows units → queue a unit → factory builds
6. Select produced units → order buttons appear → right-click to move → shift+right-click to queue
7. Fight AI units → verify combat works
8. Kill AI ACU → victory screen appears with stats
9. Return to lobby → relaunch a new game

- [ ] **Step 2: Document remaining issues**

Create a list of any non-blocking issues found during testing:
- Visual-only stubs (PlayCommanderWarpInEffect, etc.) — defer to Phase 4
- Performance issues — defer to Phase 4
- AI behavior issues — defer to Phase 2

- [ ] **Step 3: Final fixes and commit**

```bash
cmake --build build --config Debug
./build/tests/Debug/osc_tests.exe "[phase1]"
git add -A
git commit -m "Phase 1 complete: human player can build, fight, and win/lose

Human player validation confirms the core gameplay loop works:
lobby launch, construction, factory production, orders (including
shift-queue), combat, game-over detection, score screen, and
return-to-lobby for replay."
```

---

## Summary

| Task | What it fixes | Key file |
|------|--------------|----------|
| 1 | Shift-queue commands | input_handler.cpp |
| 2 | Category filter bindings (FilterOut/FilterDown) for construction panel | moho_bindings.cpp |
| 3 | ACU death → army defeat | sim_state.cpp |
| 4 | Real unit count in score screen | moho_bindings.cpp |
| 5 | Smoke test triage (discover unknown issues) | various |
| 6 | Construction panel end-to-end | moho_bindings.cpp |
| 7 | Factory production end-to-end | moho_bindings.cpp |
| 8 | Orders panel + GetUnitCommandDataOfUnit | moho_bindings.cpp |
| 9 | Economy feedback (mass/energy bars, stalling) | moho_bindings.cpp |
| 10 | Game-over → score → lobby flow | main.cpp |
| 11 | Full integration validation | all |

**Deliverable:** Human can build a base, produce units, issue commands with shift-queue, fight, reach victory/defeat screen, and return to lobby to play again.
