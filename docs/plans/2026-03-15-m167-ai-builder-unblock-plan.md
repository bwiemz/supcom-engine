# M167: AI Builder Unblock — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get the adaptive AI to build its initial base (extractors, factories, units) by fixing the OnBeginSession stub that masks real Lua initialization, fixing GetCurrentUnits category filtering, and adding diagnostic tooling.

**Architecture:** The primary fix is removing the C++ no-op `OnBeginSession` stub from the brain Lua table (session_manager.cpp:400-403) so the adaptive-ai's metatable method fires during `BeginSession()`. This unblocks GridReclaim, NavUtils, and other subsystems that initialize in `OnBeginSession`. Secondary fixes: category-filtered `GetCurrentUnits`, GridReclaim fallback stub, diagnostic entity breakdown in smoke test.

**Tech Stack:** C++17, Lua 5.0, spdlog, Catch2

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/lua/session_manager.cpp` | Modify | Remove OnBeginSession no-op stub from brain table |
| `src/lua/moho_bindings.cpp` | Modify | Fix GetCurrentUnits category filter, add GridReclaim fallback stub |
| `src/main.cpp` | Modify | Add --builder-debug flag, entity count breakdown, per-army reporting |
| `tests/test_smoke_harness.cpp` | Modify | Add test for category-filtered GetCurrentUnits (if testable without full sim) |

---

## Chunk 1: Primary Fix — OnBeginSession Unmasking

### Task 1: Remove OnBeginSession No-Op Stub

The engine sets a C++ no-op directly on each brain's Lua table at session_manager.cpp:400-403:
```cpp
lua_pushstring(L, "OnBeginSession");
lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
lua_rawset(L, brain_tbl);
```

This masks the adaptive-ai's `OnBeginSession` metatable method. When FA's `BeginSession()` calls `brain:OnBeginSession()`, the direct field (no-op) takes precedence over the metatable method that initializes GridReclaim, NavUtils, GridBrain, etc.

**Files:**
- Modify: `src/lua/session_manager.cpp` (lines ~400-403)

- [ ] **Step 1: Read session_manager.cpp and locate the OnBeginSession stub**

Read `src/lua/session_manager.cpp` and find where `OnBeginSession` is set on the brain table. It should be around lines 400-403, in the function that creates brain Lua tables.

- [ ] **Step 2: Remove the no-op stub**

Delete the 3 lines that set `OnBeginSession` as a no-op C function on the brain table:
```cpp
// REMOVE these lines:
lua_pushstring(L, "OnBeginSession");
lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
lua_rawset(L, brain_tbl);
```

Without this direct field, the Lua metatable lookup (`__index`) will find the adaptive-ai's real `OnBeginSession` method when `BeginSession()` calls it.

- [ ] **Step 3: Check for other critical no-op stubs that may mask Lua methods**

While reading the brain table creation code, check if there are other methods set directly on the table that could mask metatable methods. Look for methods that the adaptive-ai overrides. Common suspects:
- `OnBeginSession` (the one we're removing)
- `OnDestroy`, `OnDefeat`, `OnVictory` — check if these are stubs too

If any others are found that are also defined in the adaptive-ai metatable, remove them too. Document which ones you removed.

**IMPORTANT:** Do NOT remove stubs for methods that are NOT overridden by the Lua metatable. Only remove stubs where the Lua-side has a real implementation.

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All 96 tests pass. No behavioral change for tests (they don't exercise BeginSession).

- [ ] **Step 5: Commit**

```bash
git add src/lua/session_manager.cpp
git commit -m "Remove OnBeginSession no-op stub that masked adaptive-ai metatable method"
```

---

### Task 2: Validate OnBeginSession Fires with Smoke Test

Run the full smoke test to see if the OnBeginSession fix changes the AI behavior.

**Files:**
- No code changes — validation only

- [ ] **Step 1: Run the full smoke test**

```bash
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --full-smoke-test 2>&1 | \
  grep -E "Phase|tick |entity|entities|GridReclaim|OnBeginSession|NavUtils|BuilderManager|ReclaimAvailableInGrid|factory|Factory|error|Error" | tail -40
```

- [ ] **Step 2: Check results**

Look for:
- **ReclaimAvailableInGrid warnings** — should be GONE (GridReclaim now initializes via OnBeginSession)
- **Entity count changes** — if AI now builds, entity count should increase past the initial 5307
- **Any new errors** — OnBeginSession may expose new issues (missing imports, moho methods)

- [ ] **Step 3: Document findings**

Record what changed:
- Did ReclaimAvailableInGrid warnings disappear?
- Did entity counts increase (AI building)?
- What new errors appeared (if any)?

If new blocking errors appear, they become follow-up tasks in this plan. If the AI starts building, we've found the primary blocker.

---

### Task 2b: Verify moho.aibrain_methods Upvalue Timing

The adaptive-ai's `EconomyComponent` (`/lua/aibrains/components/economy.lua:16-18`) upvalues moho methods at import time:
```lua
local GetEconomyIncome = moho.aibrain_methods.GetEconomyIncome
```
If `moho.aibrain_methods` is nil when this module loads (during `OnCreateAI`), the economy thread dies silently and `EconomyOverTimeCurrent.MassIncome` stays at 0, blocking `MassToFactoryRatioBaseCheck`.

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (temporarily, for verification)

- [ ] **Step 1: Add diagnostic log to moho registration**

In the function that registers `moho.aibrain_methods` (search for where the `moho` global table is created and `aibrain_methods` is set on it), add a temporary log:
```cpp
spdlog::info("moho.aibrain_methods registered with {} methods", method_count);
```

- [ ] **Step 2: Run smoke test and check ordering**

```bash
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --full-smoke-test 2>&1 | \
  grep -iE "moho.aibrain_methods|OnCreateAI|OnCreateArmy|EconomyComponent|economy" | head -20
```

Verify that `moho.aibrain_methods registered` appears BEFORE any `OnCreateAI` or `OnCreateArmyBrain` messages. If it does, the upvalues resolve correctly. If not, the registration order needs fixing.

- [ ] **Step 3: Fix if broken**

If moho registration happens after brain creation, move the `register_moho_bindings()` call earlier in the boot sequence (before `create_army_brain` calls). This is in `sim_loader.cpp` or `session_manager.cpp`.

- [ ] **Step 4: Remove temporary diagnostic log and commit if changes were needed**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Verify/fix moho.aibrain_methods registration timing for economy thread"
```

---

## Chunk 2: GetCurrentUnits Category Filter

### Task 3: Fix GetCurrentUnits to Accept Category Filter

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (line ~4546)

- [ ] **Step 1: Read the current GetCurrentUnits implementation**

Read `src/lua/moho_bindings.cpp` at line 4546. The current implementation ignores the category argument.

- [ ] **Step 2: Replace with category-filtered version**

Replace `brain_GetCurrentUnits` with:

```cpp
static int brain_GetCurrentUnits(lua_State* L) {
    auto* brain = check_brain(L);
    auto* sim = get_sim(L);
    if (!brain || !sim) { lua_pushnumber(L, 0); return 1; }

    // Optional category filter (arg 2 — Lua category expression table)
    if (lua_istable(L, 2)) {
        int cat_idx = 2;
        i32 count = 0;
        sim->entity_registry().for_each([&](const sim::Entity& e) {
            if (e.army() == brain->index() && !e.destroyed() && e.is_unit()) {
                auto* u = static_cast<const sim::Unit*>(&e);
                if (osc::lua::unit_matches_category(L, cat_idx, u->categories()))
                    count++;
            }
        });
        lua_pushnumber(L, count);
    } else {
        // No category filter — return total unit count
        lua_pushnumber(L, brain->get_unit_cost_total(sim->entity_registry()));
    }
    return 1;
}
```

**Make sure** `#include "lua/category_utils.hpp"` is present at the top of moho_bindings.cpp (it likely already is since `unit_matches_category` is used elsewhere in the file).

- [ ] **Step 3: Build and test**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Fix GetCurrentUnits to accept category filter table (was ignoring arg)"
```

---

## Chunk 3: GridReclaim Fallback and Diagnostics

### Task 4: Add GridReclaim Fallback Stub

If `OnBeginSession` fires correctly (Task 1), FA's own Lua code initializes GridReclaim. But if the Lua-side import fails, we need a fallback to prevent `ReclaimAvailableInGrid` warnings.

**Files:**
- Modify: `src/lua/moho_bindings.cpp` or `src/lua/session_manager.cpp`

- [ ] **Step 1: Check if GridReclaim is now set after OnBeginSession fix**

After Task 2's validation, check if the `ReclaimAvailableInGrid` warnings are gone. If yes, skip this task — the Lua-side GridReclaim works.

If warnings persist, continue to Step 2.

- [ ] **Step 2: Add GridReclaim fallback stub after BeginSession**

In `session_manager.cpp`, after `call_begin_session(L)` returns (line ~80), iterate brains and check if `GridReclaim` is set. If not, install a minimal stub:

```cpp
// After BeginSession, install GridReclaim fallback for brains that don't have it
for (size_t i = 0; i < sim_state->army_count(); i++) {
    auto* brain = sim_state->army_at(i);
    if (!brain || brain->lua_table_ref() < 0) continue;

    lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
    int brain_tbl = lua_gettop(L);

    lua_pushstring(L, "GridReclaim");
    lua_rawget(L, brain_tbl);
    bool has_grid_reclaim = !lua_isnil(L, -1);
    lua_pop(L, 1);

    if (!has_grid_reclaim) {
        spdlog::debug("Installing GridReclaim stub for army {}", i);
        lua_pushstring(L, "GridReclaim");
        lua_newtable(L);

        lua_pushstring(L, "ToGridSpace");
        lua_pushcfunction(L, [](lua_State* L) -> int {
            lua_pushnumber(L, 1);
            lua_pushnumber(L, 1);
            return 2;
        });
        lua_rawset(L, -3);

        lua_pushstring(L, "MaximumInRadius");
        lua_pushcfunction(L, [](lua_State* L) -> int {
            lua_newtable(L);
            lua_pushstring(L, "TotalMass");
            lua_pushnumber(L, 0);
            lua_rawset(L, -3);
            return 1;
        });
        lua_rawset(L, -3);

        lua_rawset(L, brain_tbl);
    }

    lua_pop(L, 1); // brain_tbl
}
```

- [ ] **Step 3: Build and test**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/lua/session_manager.cpp
git commit -m "Add GridReclaim fallback stub for brains missing it after BeginSession"
```

---

### Task 5: Add Entity Count Breakdown to Smoke Test

**Files:**
- Modify: `src/main.cpp` (full_smoke_test GAME phase progress logging)

- [ ] **Step 1: Find the progress logging block**

In `src/main.cpp`, find the `--full-smoke-test` GAME phase tick loop. The progress logging is inside the `if ((t + 1) % 250 == 0)` block (around line 1676).

- [ ] **Step 2: Replace simple entity count with per-army breakdown**

Replace the progress logging with:

```cpp
if ((t + 1) % 250 == 0) {
    osc::i32 prop_count = 0;
    std::string army_summary;

    if (sim_state) {
        // Count props
        sim_state->entity_registry().for_each(
            [&](const osc::sim::Entity& e) {
                if (!e.destroyed() && !e.is_unit()) prop_count++;
            });

        // Per-army breakdown
        for (size_t a = 0; a < sim_state->army_count(); a++) {
            auto* brain = sim_state->army_at(a);
            if (!brain || brain->is_civilian()) continue;

            osc::i32 units = 0, structures = 0;
            sim_state->entity_registry().for_each(
                [&](const osc::sim::Entity& e) {
                    if (e.army() == static_cast<osc::i32>(a) &&
                        !e.destroyed() && e.is_unit()) {
                        auto* u = static_cast<const osc::sim::Unit*>(&e);
                        if (u->has_category("STRUCTURE"))
                            structures++;
                        else
                            units++;
                    }
                });

            if (!army_summary.empty()) army_summary += ", ";
            army_summary += fmt::format("a{}: {}u/{}s", a, units, structures);
        }
    }

    spdlog::info("  tick {}/1000 — props:{} {}", t + 1, prop_count, army_summary);
}
```

This produces output like: `tick 250/1000 — props:5299 a0: 1u/0s, a1: 3u/2s`

**Note:** Check if `fmt::format` is available (spdlog bundles it). If not, use `std::to_string` concatenation or spdlog::info with multiple args.

- [ ] **Step 3: Add end-of-GAME summary**

After the 1000-tick loop completes (before Phase 4), add:

```cpp
// End-of-game army summary
if (sim_state) {
    for (size_t a = 0; a < sim_state->army_count(); a++) {
        auto* brain = sim_state->army_at(a);
        if (!brain || brain->is_civilian()) continue;

        osc::i32 units = 0, structures = 0;
        sim_state->entity_registry().for_each(
            [&](const osc::sim::Entity& e) {
                if (e.army() == static_cast<osc::i32>(a) &&
                    !e.destroyed() && e.is_unit()) {
                    auto* u = static_cast<const osc::sim::Unit*>(&e);
                    if (u->has_category("STRUCTURE"))
                        structures++;
                    else
                        units++;
                }
            });

        spdlog::info("  Army {} ({}): {} units, {} structures, kills={:.0f} built={:.0f}",
                     a, brain->name(), units, structures,
                     brain->get_stat("Units_Killed"),
                     brain->get_stat("Units_Built"));
    }
}
```

- [ ] **Step 4: Add chronological warning capture**

At the top of the `--full-smoke-test` block (near `harness.activate()`), add a warning capture vector:

```cpp
std::vector<std::string> captured_warnings;
std::unordered_set<std::string> seen_warnings;
auto old_sink = spdlog::default_logger()->sinks();
// We'll capture warnings by checking after each phase instead of intercepting spdlog.
```

Actually, the simplest approach: after each phase, check the harness report for new PCALL_ERROR entries — those capture Lua errors. For non-Lua warnings (like ReclaimAvailableInGrid), they go through spdlog::warn which the harness doesn't capture.

Alternative simpler approach: after the GAME phase loop, print a note about checking the log for warnings:
```cpp
spdlog::info("  (Check full log output for warnings — grep for 'warn' to find issues)");
```

The `--builder-debug` flag (Task 6) already enables verbose logging. Combined with the smoke harness PCALL_ERROR capture, this covers most diagnostic needs without a custom warning interceptor.

- [ ] **Step 5: Build and test**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "Add per-army entity breakdown (units/structures) to --full-smoke-test progress"
```

---

### Task 6: Add --builder-debug Flag

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add flag parsing**

In the flag parsing section of main.cpp (near the other `--` flags), add:

```cpp
bool builder_debug = parse_flag(argc, argv, "--builder-debug");
```

- [ ] **Step 2: Enable debug logging when flag is set**

After spdlog is initialized but before sim boot, add:

```cpp
if (builder_debug) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("Builder debug mode enabled — verbose logging active");
}
```

This enables all existing `spdlog::debug` calls in moho methods (BuildStructure, FindPlaceToBuild, IssueBuildMobile, etc.).

- [ ] **Step 3: Build and test**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Add --builder-debug flag to enable verbose AI builder logging"
```

---

### Task 7: Run Full Validation and Fix Follow-Up Issues

This is an iterative task — run the smoke test, observe results, fix any new blockers that appear.

**Files:**
- Various (depends on what issues surface)

- [ ] **Step 1: Run the full smoke test with builder debug**

```bash
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe \
  --map "/maps/SCMP_009/SCMP_009_scenario.lua" --full-smoke-test --builder-debug 2>&1 | \
  grep -iE "Phase|tick |army|factory|structure|build|error|warn|GridReclaim|OnBeginSession|economy|mass" | tail -60
```

- [ ] **Step 2: Assess results**

Check:
1. **Entity breakdown** — do per-army structure counts increase? (AI building)
2. **ReclaimAvailableInGrid** — warnings gone?
3. **New errors** — any new errors from OnBeginSession running?
4. **Economy** — is mass income > 0 for AI armies?
5. **BaseSettings** — check if `BuilderManagers['MAIN'].BaseSettings.FactoryCount` is populated. The `FactoryCapCheck` condition reads `BaseSettings.FactoryCount.Land`. If nil, the condition silently returns false and no factories are built. This depends on `AddGlobalBaseTemplate` running correctly, which requires `BeginSessionAI` to have loaded the base templates BEFORE `ExecutePlan` uses them. If the AI still doesn't build and all other checks pass, this ordering issue is the likely cause.

- [ ] **Step 3: Fix any new blocking issues discovered**

If new issues surface (e.g., missing imports in OnBeginSession, moho method upvalue failures), fix them. Common expected issues:
- **NavUtils import failure** — FA's NavUtils may need pathfinding grid access the engine doesn't expose. If it errors, the error is caught by pcall and OnBeginSession continues.
- **GridPresence/GridDeposits import failure** — same pattern. Non-fatal.
- **moho.aibrain_methods upvalue nil** — if EconomyComponent can't resolve moho methods, the economy thread dies silently. Fix by ensuring moho bindings are registered before any brain code imports economy.lua.

Each fix gets its own commit.

- [ ] **Step 4: Re-run smoke test after fixes**

Verify the fixes improved the situation. Target: at least 1 AI army building at least 1 structure.

- [ ] **Step 5: Final commit with summary of all fixes**

```bash
# Add only the specific files you modified
git add src/lua/session_manager.cpp src/lua/moho_bindings.cpp  # (adjust to actual files changed)
git commit -m "Fix follow-up issues from OnBeginSession unmasking"
```

---

### Task 8: Update README and Memory

**Files:**
- Modify: `README.md`
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/MEMORY.md`
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/milestones-list.md`

- [ ] **Step 1: Update README**

Add `--builder-debug` to the integration test flags table. Update milestone count and test counts if changed.

- [ ] **Step 2: Update milestones-list.md**

Add M167 entry.

- [ ] **Step 3: Update MEMORY.md**

Add key decisions for M167: OnBeginSession unmasking, GetCurrentUnits category filter, GridReclaim fallback, diagnostic additions.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "Update README and memory for M167 AI builder unblock"
```

---

## Summary

| Task | What it delivers | Files |
|------|-----------------|-------|
| 1 | Remove OnBeginSession no-op that masks real Lua method | session_manager.cpp |
| 2 | Validate the fix with smoke test | (validation only) |
| 2b | Verify moho.aibrain_methods upvalue timing | moho_bindings.cpp |
| 3 | GetCurrentUnits accepts category filter | moho_bindings.cpp |
| 4 | GridReclaim fallback stub (if Lua-side fails) | session_manager.cpp |
| 5 | Per-army entity breakdown in smoke test | main.cpp |
| 6 | --builder-debug flag for verbose logging | main.cpp |
| 7 | Fix follow-up issues + BaseSettings verification | various |
| 8 | README + memory updates | README.md, memory |

**Final success criteria:** `--full-smoke-test` with 1000 ticks shows at least 1 AI army building structures (structure count > 0 in per-army breakdown).
