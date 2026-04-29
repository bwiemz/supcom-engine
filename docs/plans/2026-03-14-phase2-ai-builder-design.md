# Phase 2: AI Builder Infrastructure — Design Spec

**Goal:** FA's adaptive AI autonomously builds a base, produces units, and sends them to attack — using the real FA Lua AI code, not custom shortcuts.

**Approach:** Implement the minimum set of missing moho methods (6 methods + 1 alias) that unblock FA's AI builder manager chain: FactoryBuilderManager, EngineerManager, and PlatoonFormManager.

**Prerequisites:** Phase 1 complete (human player validation, 0 smoke test issues, all construction/factory/economy bindings verified).

---

## Problem Statement

FA's AI runs through a chain of Lua managers: ExecutePlan → BuilderManager → EngineerManager / FactoryBuilderManager / PlatoonFormManager. These managers call moho methods on brain and platoon objects. Several methods are currently missing or misnamed, causing the AI threads to silently fail and never issue build orders. The AI loads, creates threads, but never builds anything.

### Current AI Thread Warnings

```
Thread error: /lua/sim/platoonformmanager.lua:141: attempt to call method 'CanFormPlatoon' (a nil value)
Thread error: /lua/editor/threatbuildconditions.lua:58: attempt to call method 'GetPlatoonThreat' (a nil value)
```

---

## Template Formats

FA uses two distinct template formats. Both share the structure `{name, plan, sub1, sub2, ...}` where entries from index 3 onward are sub-tables of `{identifier, min, max, squad, formation}`, but the identifier type differs:

### Factory Templates (BuildPlatoon / CanBuildPlatoon)

Built by `GetFactoryTemplate()` from `PlatoonTemplates[name].FactionSquads[faction]`. Each sub-table's first element is a **blueprint ID string**:

```lua
{
    'T1LandDFTank',           -- template[1] = name
    '',                        -- template[2] = plan (empty for factory builds)
    { 'uel0201', 1, 1, 'attack', 'GrowthFormation' },  -- sub-table: bp_id string
}
```

### Form Templates (CanFormPlatoon / FormPlatoon)

Defined in `platoontemplates.lua`. Each sub-table's first element is a **Lua category expression object** (compound tree with `__op`/`__left`/`__right` keys, or simple `{__name="FOO"}`):

```lua
{
    "Engineer",                                            -- template[1] = name
    "EngineerBuildAI",                                     -- template[2] = plan
    { categories.ENGINEER * categories.TECH1, 1, 3, "support", "None" },  -- category expr
}
```

Category expressions are created by `categories.*` metamethods: `*` = intersection, `+` = union, `-` = difference. The engine already has `unit_matches_category()` in `category_utils.hpp` that recursively evaluates these trees.

---

## Methods to Implement

### 1. GetPlatoonThreat / CalculatePlatoonThreatAroundPosition — Threat Query Fixes

**Problem:** FA's `platoon.lua:328` defines `GetPlatoonThreat` as a Lua wrapper that dispatches to `CalculatePlatoonThreat` (no position) or `CalculatePlatoonThreatAroundPosition` (with position/radius). The error at `threatbuildconditions.lua:58` indicates this wrapper isn't resolving, likely because the Lua class chain for platoon objects isn't fully connected.

**Fix A — GetPlatoonThreat alias:** Add `{"GetPlatoonThreat", platoon_CalculatePlatoonThreat}` to the platoon methods table. This fixes the blocking error at `threatbuildconditions.lua:58` which calls without position/radius. Keep `CalculatePlatoonThreat` for backward compat.

**Fix B — CalculatePlatoonThreatAroundPosition:** New C++ method. Same logic as `platoon_CalculatePlatoonThreat` (moho_bindings.cpp:6198) but also filters units by position/radius:
1. Args: `(threatType, category, position, radius)` — position is a `{x, y, z}` table, radius is a number
2. Iterate `platoon->unit_ids()`, filter by `unit_matches_category()` AND distance from position ≤ radius
3. Sum threat values for matching units
4. Return total as number

This ensures both the no-position and with-position call paths work regardless of Lua class resolution.

**Note:** The root cause (why `platoon.lua`'s Lua class chain isn't resolving `GetPlatoonThreat`) should be investigated as a follow-up. If the C++ `__index` table short-circuits before reaching Lua class methods, other Lua-defined platoon methods will also be unreachable. The C++ alias is a pragmatic fix that works regardless.

**Effort:** 1 line (alias) + ~25 lines (AroundPosition).

### 2. CanBuildPlatoon(template, factories)

**Object:** Brain method.
**Called by:** `FactoryBuilderManager.BuilderParamCheck` to check if factories can build requested blueprints.
**Returns:** The factories table (truthy) on success, or `false` on failure. FA's `campaign-ai.lua` passes the return value to `PBMBuildNumFactories` and `BuildPlatoon`, so it must be the factories table, not just a boolean. A non-empty table is truthy in Lua, so this also works for the boolean usage in `BuilderParamCheck`.

**Implementation:**
1. Parse template: iterate sub-tables starting at Lua index 3 (`lua_rawgeti(L, template_arg, i)` for i=3,4,...)
2. For each sub-table, extract `sub[1]` as blueprint ID string
3. Look up the blueprint's `CategoriesHash` from `__blueprints[bp_id]` to get the unit's categories
4. For each factory in the factories table, extract Unit* via `_c_object` lightuserdata, get the factory's blueprint ID via `unit->blueprint_id()`, then look up `__blueprints[bp_id].Economy.BuildableCategory` string. If `BuildableCategory` is nil, that factory cannot build the entry (skip it).
5. Parse `BuildableCategory` as a space-separated list of category tokens (intersection semantics). Check that every token appears in the requested blueprint's `CategoriesHash`.
6. If every template entry has at least one capable factory: push arg 2 (the factories table) and return 1. Otherwise: push false and return 1.

**Key detail:** `BuildableCategory` is a string like `"BUILTBYTIER1FACTORY UEF MOBILE"`. Each space-separated token is a category name. All tokens must be present in the blueprint's `CategoriesHash` for the factory to be capable. This is simpler than full category expression evaluation — it's just a string split + hash lookup.

**Effort:** ~45 lines.

### 3. BuildPlatoon(template, factories, count)

**Object:** Brain method.
**Called by:** `FactoryBuilderManager.AssignBuildOrder` to queue unit production.
**Returns:** nil (fire-and-forget).

**Implementation:**
1. Parse template sub-tables starting at index 3, same as CanBuildPlatoon
2. For each sub-table, extract blueprint ID string from `sub[1]` and count from `sub[2]` (min) × `count` arg
3. Iterate the factories table (arg 3). For each factory, extract Unit* via `_c_object` lightuserdata.
4. For each template entry, find a matching factory and issue build order using existing `brain_BuildUnit` pattern: create `UnitCommand{type=BuildFactory, blueprint_id=bp_id}` and call `unit->push_command(cmd, false)`
5. Return 0 (no return values)

**Key detail:** In the common skirmish case, `FactoryBuilderManager.AssignBuildOrder` passes `{factory}` (single-element table) with a single-entry template. The factory gets one `BuildFactory` command per call. The factory's existing `BuildFactory` handler processes start_build → progress_build → spawn. When a factory finishes one unit, FA's `FactoryFinishBuilding` Lua callback fires and calls `AssignBuildOrder` again for the next unit.

**Effort:** ~35 lines.

### 4. CanFormPlatoon(template, count, location, radius)

**Object:** Platoon method (called on ArmyPool platoon).
**Called by:** `PlatoonFormManager.ManagerLoopBody` to check if idle units can form a squad.
**Returns:** boolean.

**Implementation:**
1. Parse template sub-tables starting at Lua index 3 (arg 2 is the template table)
2. For each sub-table:
   - Extract the category expression object from `sub[1]` (a Lua table, NOT a string)
   - Extract min from `sub[2]`, max from `sub[3]`
   - Count pool units (from `platoon->unit_ids()`) whose categories match using `unit_matches_category(L, cat_idx, unit->categories())`
   - If location (arg 4) and radius (arg 5) provided, also filter by distance from location
   - If count < min × `count` arg (arg 3), return false
3. Return true if all template entries satisfied

**Category matching:** Uses existing `unit_matches_category()` from `category_utils.hpp`, which recursively evaluates compound category expressions (`__op`=union/intersection/difference). Same function already used by `platoon_CalculatePlatoonThreat` and `brain_GetNumUnitsAroundPoint`.

**Effort:** ~50 lines.

### 5. FormPlatoon(template, count, location, radius)

**Object:** Platoon method (called on ArmyPool platoon).
**Called by:** `PlatoonFormManager.ManagerLoopBody` to extract units from pool into new platoon.
**Returns:** New platoon Lua handle.

**Implementation:**
1. Get brain from platoon's `army_index_`, call `brain->create_platoon()` to make a new empty platoon
2. Parse template sub-tables starting at index 3, same as CanFormPlatoon
3. For each sub-table:
   - Extract category expression from `sub[1]`, min from `sub[2]`, max from `sub[3]`, squad from `sub[4]`
   - Walk pool's `unit_ids_`, find units matching categories via `unit_matches_category()`
   - If location/radius provided, also filter by distance
   - Transfer up to max × count units (at least min × count) into new platoon via `new_platoon->add_unit()` and `pool->remove_unit()`
   - Set each transferred unit's squad assignment via `new_platoon->set_unit_squad(id, squad)`
4. Set new platoon's `plan_name_` from `template[2]`
5. Create Lua table for the platoon with `_c_object` lightuserdata (same pattern as `brain_MakePlatoon`)
6. Return 1 (the platoon Lua table)

**Key detail:** Must copy `unit_ids_` before iterating (`auto ids = pool->unit_ids();`) since `remove_unit()` modifies the vector during traversal.

**Effort:** ~70 lines.

### 6. CheckBlockingTerrain — No-op Stub

**Object:** Brain method.
**Called by:** `aiattackutilities.lua:492,1611` for attack path validation.
**Returns:** false (no blocking terrain detected).

**Implementation:** Single-line stub returning false. The AI will attempt attacks regardless and fail gracefully if terrain blocks. Not critical for AI builder unblocking — primarily affects attack platoon pathing decisions.

**Effort:** 5 lines.

---

## What We're NOT Implementing

| Method | Reason |
|--------|--------|
| CombinePlatoons | Pure Lua method defined in `campaign-ai.lua:1702`. Uses existing moho methods (`MakePlatoon`, `GetSquadUnits`, `AssignUnitsToPlatoon`, `DisbandPlatoon`). Only used by campaign AI's `AttackManager`. Not in skirmish critical path. |
| PBMAddBuildLocation / PBMRemoveBuildLocation | Campaign-only (called by campaign-ai.lua, not skirmish brains). Left as existing no-op stubs. |
| Spatial threat grid | Premature optimization. Existing query-based GetThreatAtPosition works. Defer to Phase 4 if profiling shows need. |
| Pathfinding time-slicing | Phase 4 concern. Not needed for AI builder unblocking. |

## Already Implemented (Verified)

| Method | Status |
|--------|--------|
| MakePlatoon | Real impl (moho_bindings.cpp:5489) |
| DisbandPlatoon | Real impl (moho_bindings.cpp:5637) |
| AssignUnitsToPlatoon | Real impl |
| GetPlatoonUniquelyNamed | Real impl |
| GetPlatoonUnits / GetSquadUnits | Real impl |
| GetPlatoonPosition | Real impl |
| SetPlatoonFormationOverride | Real impl |
| GetNumUnitsAroundPoint | Real impl (moho_bindings.cpp:5241) |
| GetUnitBlueprint | Real impl (moho_bindings.cpp:5470) |
| DecideWhatToBuild | Real impl (moho_bindings.cpp:5443) |
| SetResourceSharing | Real impl (moho_bindings.cpp:6385) |
| BuildUnit | Real impl (moho_bindings.cpp:5417) |
| BuildStructure | Real impl (moho_bindings.cpp:5383) |
| CalculatePlatoonThreat | Real impl (moho_bindings.cpp:6198) |

---

## Files Modified

- `src/lua/moho_bindings.cpp` — all 6 implementations + 1 alias, registered in existing platoon/brain method tables

No new files. All methods follow existing patterns:
- `check_brain(L)` / `check_platoon(L)` for self validation
- `_c_object` lightuserdata for entity/platoon extraction from Lua tables
- `unit_matches_category()` from `category_utils.hpp` for category expression matching
- `lua_rawgeti` for template sub-table parsing
- `UnitCommand` + `push_command()` for build orders

---

## Testing & Validation

### Unit Tests
- CanFormPlatoon returns true/false based on pool contents and category matching
- FormPlatoon transfers units between platoons, removes from pool, sets squad assignments
- BuildPlatoon issues BuildFactory commands to factories via push_command
- CanBuildPlatoon returns factories table on success, false on failure
- CalculatePlatoonThreatAroundPosition filters by position/radius

### Integration Validation
Run `--ai-skirmish --ticks 3000` and verify:
- Thread warning count for `CanFormPlatoon` / `GetPlatoonThreat` drops to 0
- AI builds at least 1 factory within 1000 ticks
- AI produces at least 1 unit within 2000 ticks
- AI sends a platoon to attack within 3000 ticks

### Success Criteria
Two AI armies load, build bases, produce units, and engage in combat. The `--ai-skirmish` log shows factory construction, unit production, and platoon movement.
