# M167: AI Builder Unblock — Design Spec

**Date:** 2026-03-15
**Goal:** Get the adaptive AI to build its initial base (extractors, factories, units) and expand, with diagnostic logging to identify and fix remaining blockers.

**Motivation:** After M166's smoke test, the AI spawns 8 ACUs but builds nothing in 1000 ticks. The builder condition evaluation chain runs (evidenced by `ReclaimAvailableInGrid` warnings) but all builders fail. This milestone addresses the known gaps and adds diagnostics to surface unknown ones.

**Relationship to Phase 2:** The Phase 2 AI builder design/plan (M163) implemented the core moho methods (BuildStructure, FindPlaceToBuild, CanBuildStructureAt, platoon management, threat queries, etc.). Those are complete. M167 addresses **runtime unblocking** — making the existing FA Lua code actually execute successfully end-to-end.

---

## Current State

The AI initialization chain works through M163:
- `SetArmyPlans` → stores PlanName on brain
- `InitializeArmyAI` → calls `brain:OnCreateAI(planName)`
- `OnCreateAI` → sets adaptive-ai metatable, calls `CreateBrainShared`
- `CreateBrainShared` → creates ConditionsMonitor, calls `InitializeSkirmishSystems`
- `InitializeSkirmishSystems` → creates `BuilderManagers['MAIN']` with FactoryManager, EngineerManager, PlatoonFormManager
- `ExecutePlan` → loads base templates, adds builders to managers
- Builder evaluation threads run → conditions are checked → **all fail → nothing built**

---

## Likely Root Causes (Priority Order)

### Cause A: `moho.aibrain_methods` Upvalue Resolution

The adaptive-ai's `EconomyComponent` (`/lua/aibrains/components/economy.lua:16-18`) upvalues moho methods at import time:
```lua
local GetEconomyIncome = moho.aibrain_methods.GetEconomyIncome
local GetEconomyRequested = moho.aibrain_methods.GetEconomyRequested
local GetEconomyTrend = moho.aibrain_methods.GetEconomyTrend
```

If `moho.aibrain_methods` is nil when this module first loads (during `OnCreateAI`), all three upvalues are nil and the EconomyUpdateThread silently crashes. This would leave `EconomyOverTimeCurrent.MassIncome` at 0 (or nil), causing `MassToFactoryRatioBaseCheck` to return false for every builder.

**Verification:** Check if `moho.aibrain_methods` is populated before `OnCreateAI` runs. If not, this is the primary blocker.

### Cause B: `OnBeginSession` Not Firing

The adaptive-ai sets up critical subsystems in `OnBeginSession` (adaptive-ai.lua:147):
- `GridReclaim = import("/lua/ai/gridreclaim.lua").Setup(self)`
- `GridBrain`, `GridRecon`, `GridDeposits`, `GridPresence`
- NavUtils, marker utilities

If the engine's `BeginSession` call doesn't trigger `OnBeginSession` on brain objects, ALL of these are nil. The `ReclaimAvailableInGrid` warning is a symptom of this.

**Verification:** Check if `BeginSession` in the engine calls `OnBeginSession` on each brain's Lua table. If not, this is a major blocker that affects far more than just GridReclaim.

### Cause C: `GetCurrentUnits` Category Filter Missing

`brain_GetCurrentUnits` (moho_bindings.cpp:4546) ignores its category argument and returns total unit count. FA calls `aiBrain:GetCurrentUnits(categories.TECH1 * categories.FACTORY)` expecting a filtered count. This doesn't block the *first* factory (the unfiltered count makes the math work accidentally), but breaks factory cap tracking after the first factory is built, causing the AI to stop expanding.

### Cause D: `BaseSettings` Not Initialized

`FactoryCapCheck` reads `BuilderManagers['MAIN'].BaseSettings.FactoryCount.Land`. If the base template loading in `ExecutePlan` fails silently, `BaseSettings` is nil and this condition returns false. Need to verify the template loading chain works.

---

## Changes

### 1. Verify and Fix `OnBeginSession` Firing

**Problem:** The adaptive-ai expects `OnBeginSession` to be called on each brain after `BeginSession`. This is where GridReclaim, NavUtils, and other subsystems are initialized.

**Verification step:** Search the engine for where `BeginSession` is handled in `session_manager.cpp` or `sim_state.cpp`. Check if it iterates brains and calls their `OnBeginSession` Lua method.

**Note:** FA's own `BeginSession()` in `simInit.lua:348-349` already iterates `ArmyBrains` and calls `brain:OnBeginSession()`. The engine calls this via `call_begin_session()` in `session_manager.cpp`. So if `BeginSession()` succeeds, `OnBeginSession` fires automatically.

**Fix strategy:** First verify that `BeginSession()` executes successfully (check for pcall errors in the logs). If it does, `OnBeginSession` should already fire and no C++ fallback is needed. If `BeginSession()` fails (e.g., a Lua error), fix the root cause of that failure rather than adding a redundant C++ loop that could double-fire `OnBeginSession` and cause double-initialization of GridReclaim/NavUtils.

**Diagnostic:** Add a temporary `spdlog::info` in the engine's `call_begin_session()` to log whether it succeeds or fails, and check if the `OnBeginSession` path runs (look for GridReclaim initialization logs).

This is the highest-priority verification because it unblocks GridReclaim, NavUtils, and several other subsystems at once.

### 2. Verify `moho.aibrain_methods` Timing

**Problem:** Economy component upvalues `moho.aibrain_methods.GetEconomyIncome` at import time. If `moho` table isn't populated yet, the economy thread dies silently and `EconomyOverTimeCurrent` stays empty.

**Verification step:** Add a temporary log in the engine's moho registration code to confirm `moho.aibrain_methods` is set before any brain's `OnCreateAI` runs. The registration order in `boot_sim()` should be: register moho bindings → load system Lua → create brains → call OnCreateAI.

**Fix if broken:** Ensure moho bindings are registered before any brain initialization. This is likely already correct (M163 established this order), but verify.

### 3. Fix `GetCurrentUnits` Category Filter

**Problem:** FA passes category expression tables (compound Lua tables with `__op`/`__left`/`__right` keys), not integer bitmasks. Categories are stored as `std::unordered_set<std::string>` on Unit.

**Fix:** Follow the existing `brain_GetNumUnitsAroundPoint` pattern (moho_bindings.cpp ~line 5360) which already does category-filtered unit iteration:

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
        // No category filter — return total unit count for this army.
        // (get_unit_cost_total is misnamed — it returns a count, not cost.)
        lua_pushnumber(L, brain->get_unit_cost_total(sim->entity_registry()));
    }
    return 1;
}
```

**Dependency:** Uses existing `osc::lua::unit_matches_category(L, cat_idx, categories)` which is already used by `brain_GetNumUnitsAroundPoint` and other methods.

### 4. Add `GridReclaim` Stub (Fallback)

**Problem:** If `OnBeginSession` fires correctly (Change #1), GridReclaim is set up by FA's own Lua code. But if the Lua-side setup fails (e.g., import error), we need a fallback.

**Fix:** If `brain.GridReclaim` is still nil after `OnBeginSession`, set a minimal C++ stub. The actual FA code (`MiscBuildConditions.lua:370-373`) calls:
- `gridReclaim:ToGridSpace(x, z)` → returns two grid coordinates
- `gridReclaim:MaximumInRadius(bx, bz, rings)` → returns `{TotalMass = number}`

Minimal stub:
```cpp
// GridReclaim stub table
lua_pushstring(L, "GridReclaim");
lua_rawget(L, brain_tbl);
if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushstring(L, "GridReclaim");
    lua_newtable(L);

    lua_pushstring(L, "ToGridSpace");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        lua_pushnumber(L, 1); lua_pushnumber(L, 1);
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
} else {
    lua_pop(L, 1);
}
```

This makes `ReclaimAvailableInGrid` return false cleanly (no reclaim = no reclaim-based builders activate) instead of spamming warnings.

### 5. Diagnostic Logging

**a) `--builder-debug` CLI flag:**
Add the flag to main.cpp. When set, enable `spdlog::set_level(spdlog::level::debug)` so all existing debug-level logs in moho methods fire (BuildStructure, FindPlaceToBuild, IssueBuildMobile already have debug logs).

**b) Engine-side builder diagnostics:**
Add `spdlog::info` calls to these key methods (only when builder_debug is active):
- `brain_BuildStructure` — log blueprint ID + position
- `brain_FindPlaceToBuild` — log result (success/fail + coordinates)
- `brain_GetCurrentUnits` — log category filter result count (first few calls only)

**c) Chronological error capture:**
During `--full-smoke-test` and `--ai-skirmish`, capture the first 50 unique `spdlog::warn` messages in chronological order. Print them at the end of the test alongside the smoke report. This surfaces the FIRST errors, not just the most frequent.

**d) Tick-based entity summary:**
In the smoke test progress logging (every 250 ticks), break down entities by army:
```
tick 250/1000 — 5307 total (props: 5299, army0: 1u/0s, army1: 1u/0s, ...)
```
Where `u` = units, `s` = structures. At the end of GAME phase, report per-army stats including economy.

### 6. Verify BaseSettings Initialization

**Problem:** `FactoryCapCheck` reads `BuilderManagers['MAIN'].BaseSettings.FactoryCount.Land`. If `BaseSettings` is nil, the condition silently returns false.

**Verification:** After `ExecutePlan` runs, check that `BuilderManagers['MAIN'].BaseSettings` exists and has the expected structure from the NormalMain base template (`FactoryCount.Land = 2`).

**Fix if broken:** The base template is loaded by `AddGlobalBaseTemplate` which reads from `BaseBuilderTemplates['NormalMain']`. These templates are loaded in `BeginSessionAI`. If `BeginSessionAI` runs AFTER `ExecutePlan` tries to use them, the templates aren't loaded yet.

Fix: Ensure the ordering is `BeginSessionAI` (loads templates) → `ExecutePlan` (uses templates). This may require deferring `ExecutePlan` until after `BeginSession`. Check the engine's session start sequence and adjust if needed.

---

## What This Does NOT Cover

- Full PBM expansion system (rally points, factory assignment) — deferred
- Full GridReclaim implementation (actual reclaim grid) — stub sufficient
- Multiplayer or campaign AI — single-player skirmish only
- Air/naval factory builders — land factory first, others follow naturally
- FA Lua file modifications — all changes are engine-side

## Success Criteria

- `--full-smoke-test` with 1000 ticks shows AI armies building structures (factory count > 0)
- `--builder-debug` flag produces actionable diagnostic output
- Entity count breakdown shows per-army structures and units
- `ReclaimAvailableInGrid` warning eliminated (either real GridReclaim loads or stub installed)
- No new crashes or regressions in existing 96 tests

## Roadmap Context

| Milestone | Task | Depends On |
|-----------|------|------------|
| **M167** | AI builder unblock (this) | M166 smoke test |
| **M168** | Fix remaining stubs from smoke test | M167 diagnostics |
| **M169** | Loading screen overlay | M168 |
| **M170** | Map preview in lobby | M168 |
| **M171** | Sound completeness audit | M168 |
| **M172** | Main menu background fallback | M168 |
