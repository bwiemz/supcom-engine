# Phase 2: AI Builder Infrastructure — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement 6 moho methods + 1 alias in `src/lua/moho_bindings.cpp` to unblock FA's AI builder chain so the AI can autonomously build bases, produce units, and send platoons to attack.

**Architecture:** All implementations go in `src/lua/moho_bindings.cpp`, following existing patterns (`check_brain`/`check_platoon`, `_c_object` lightuserdata, `unit_matches_category()`). Platoon methods registered in `platoon_methods[]` table, brain methods in `aibrain_methods[]` table. No new files.

**Tech Stack:** C++17, Lua 5.0 C API, Catch2 (tests), CMake + vcpkg (build)

**Spec:** `docs/plans/2026-03-14-phase2-ai-builder-design.md`

---

## Chunk 1: Threat Methods + CheckBlockingTerrain

### Task 1: GetPlatoonThreat Alias + CalculatePlatoonThreatAroundPosition

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (insert function after line ~6224, add entries to `platoon_methods[]` at line ~6960)

- [ ] **Step 1: Add `platoon_CalculatePlatoonThreatAroundPosition` function**

Insert this function immediately after `platoon_CalculatePlatoonThreat` (after line ~6224 in `moho_bindings.cpp`):

```cpp
// platoon:CalculatePlatoonThreatAroundPosition(threatType, category, position, radius)
// Same as CalculatePlatoonThreat but filters units to within radius of position.
static int platoon_CalculatePlatoonThreatAroundPosition(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim) {
        lua_pushnumber(L, 0);
        return 1;
    }

    const char* threat_type = lua_isstring(L, 2) ? lua_tostring(L, 2) : "Overall";
    int cat_idx = lua_istable(L, 3) ? 3 : 0;

    // Extract position from arg 4 ({x, y, z} table)
    f32 px = 0, pz = 0;
    if (lua_istable(L, 4)) {
        lua_rawgeti(L, 4, 1);
        px = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 3);
        pz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    f32 radius = lua_isnumber(L, 5) ? static_cast<f32>(lua_tonumber(L, 5)) : 0;
    f32 radius_sq = radius * radius;

    f32 total = 0;
    for (u32 id : platoon->unit_ids()) {
        auto* e = sim->entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<sim::Unit*>(e);

        if (cat_idx > 0 &&
            !osc::lua::unit_matches_category(L, cat_idx, unit->categories()))
            continue;

        // Distance filter (2D, ignoring Y)
        if (radius > 0) {
            auto pos = unit->position();
            f32 dx = pos.x - px;
            f32 dz = pos.z - pz;
            if (dx * dx + dz * dz > radius_sq) continue;
        }

        total += get_unit_threat_for_type(unit, threat_type);
    }

    lua_pushnumber(L, total);
    return 1;
}
```

- [ ] **Step 2: Register both methods in `platoon_methods[]` table**

In the `platoon_methods[]` array (around line ~6960), add these two entries before the `{nullptr, nullptr}` sentinel — after the existing `CalculatePlatoonThreat` entry:

```cpp
    {"GetPlatoonThreat",                        platoon_CalculatePlatoonThreat},
    {"CalculatePlatoonThreatAroundPosition",    platoon_CalculatePlatoonThreatAroundPosition},
```

- [ ] **Step 3: Build and verify compilation**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5
```
Expected: Build succeeds with no errors.

- [ ] **Step 4: Run existing tests to verify no regressions**

Run:
```bash
./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: All existing tests pass (91+ tests, 1500+ assertions).

- [ ] **Step 5: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add GetPlatoonThreat alias and CalculatePlatoonThreatAroundPosition

GetPlatoonThreat: alias for existing CalculatePlatoonThreat — fixes
thread error at threatbuildconditions.lua:58 where FA calls
GetPlatoonThreat but engine registered it as CalculatePlatoonThreat.

CalculatePlatoonThreatAroundPosition: same threat summation logic
but filters platoon units to within radius of a given position.
Called by platoon.lua:GetPlatoonThreat wrapper for position-based
threat queries.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: CheckBlockingTerrain No-op Stub

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (insert function near brain methods, add entry to `aibrain_methods[]`)

- [ ] **Step 1: Add `brain_CheckBlockingTerrain` stub**

Insert this function near the other brain method stubs in `moho_bindings.cpp` (before the `aibrain_methods[]` table):

```cpp
// brain:CheckBlockingTerrain(pos, maxRange, threatType)
// No-op stub — returns false (no blocking terrain).
// Called by aiattackutilities.lua for attack path validation.
static int brain_CheckBlockingTerrain(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
```

- [ ] **Step 2: Register in `aibrain_methods[]` table**

Add this entry to the `aibrain_methods[]` array (around line ~6480), before the `{nullptr, nullptr}` sentinel:

```cpp
    {"CheckBlockingTerrain",    brain_CheckBlockingTerrain},
```

- [ ] **Step 3: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add CheckBlockingTerrain no-op stub for AI attack utilities

Returns false (no blocking terrain detected). Called by
aiattackutilities.lua for attack path validation. Full terrain
pathfinding check deferred to Phase 4.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Chunk 2: Factory Builder Methods

### Task 3: CanBuildPlatoon

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (insert function near `brain_BuildUnit`, add entry to `aibrain_methods[]`)

**Reference:**
- `brain_BuildUnit` at line ~5419 shows the pattern for extracting Unit* from factory Lua tables
- `BuildableCategory` is on the factory's blueprint at `Economy.BuildableCategory` — a Lua TABLE of space-separated category strings
- Each string is an intersection: all tokens must appear in the requested blueprint's `CategoriesHash`
- If ANY string in the table matches, the factory can build the blueprint

- [ ] **Step 1: Add `#include <sstream>` if not present**

Check the includes at the top of `src/lua/moho_bindings.cpp`. If `<sstream>` is not already included, add it near the other standard library includes:

```cpp
#include <sstream>
```

- [ ] **Step 2: Add `brain_CanBuildPlatoon` function**

Insert after `brain_BuildUnit` (after line ~5437):

```cpp
// brain:CanBuildPlatoon(template, factories)
// template = {name, plan, {bp_id, min, max, squad, formation}, ...}
// factories = {factory1, factory2, ...}
// Returns: factories table (truthy) on success, false on failure.
static int brain_CanBuildPlatoon(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain || !lua_istable(L, 2) || !lua_istable(L, 3)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int tmpl = 2;
    int facs = 3;

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = blueprint ID string
        lua_rawgeti(L, sub, 1);
        if (!lua_isstring(L, -1)) { lua_pop(L, 2); continue; }
        std::string bp_id = lua_tostring(L, -1);
        lua_pop(L, 1);

        // Look up blueprint's CategoriesHash
        std::unordered_set<std::string> bp_cats;
        lua_pushstring(L, "__blueprints");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, bp_id.c_str());
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "CategoriesHash");
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushnil(L);
                    while (lua_next(L, -2) != 0) {
                        if (lua_type(L, -2) == LUA_TSTRING)
                            bp_cats.insert(lua_tostring(L, -2));
                        lua_pop(L, 1); // pop value, keep key
                    }
                }
                lua_pop(L, 1); // CategoriesHash or nil
            }
            lua_pop(L, 1); // bp table or nil
        }
        lua_pop(L, 1); // __blueprints

        if (bp_cats.empty()) {
            lua_pop(L, 1); // sub
            lua_pushboolean(L, 0);
            return 1;
        }

        // Check if any factory can build this blueprint
        bool found = false;
        for (int f = 1; !found; f++) {
            lua_rawgeti(L, facs, f);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            // Get factory Unit*
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            auto* e = lua_isuserdata(L, -1)
                          ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                          : nullptr;
            lua_pop(L, 1); // _c_object

            if (!e || !e->is_unit() || e->destroyed()) {
                lua_pop(L, 1); // factory table
                continue;
            }

            // Look up factory blueprint's Economy.BuildableCategory
            lua_pushstring(L, "__blueprints");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, e->blueprint_id().c_str());
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "Economy");
                    lua_rawget(L, -2);
                    if (lua_istable(L, -1)) {
                        lua_pushstring(L, "BuildableCategory");
                        lua_rawget(L, -2);
                        if (lua_istable(L, -1)) {
                            // Table of category strings
                            int bc = lua_gettop(L);
                            for (int b = 1; !found; b++) {
                                lua_rawgeti(L, bc, b);
                                if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
                                if (lua_isstring(L, -1)) {
                                    std::string pattern = lua_tostring(L, -1);
                                    bool match = true;
                                    std::istringstream ss(pattern);
                                    std::string token;
                                    while (ss >> token) {
                                        if (bp_cats.count(token) == 0) {
                                            match = false;
                                            break;
                                        }
                                    }
                                    if (match) found = true;
                                }
                                lua_pop(L, 1); // entry
                            }
                        } else if (lua_isstring(L, -1)) {
                            // Single string fallback
                            std::string pattern = lua_tostring(L, -1);
                            bool match = true;
                            std::istringstream ss(pattern);
                            std::string token;
                            while (ss >> token) {
                                if (bp_cats.count(token) == 0) {
                                    match = false;
                                    break;
                                }
                            }
                            if (match) found = true;
                        }
                        lua_pop(L, 1); // BuildableCategory
                    }
                    lua_pop(L, 1); // Economy
                }
                lua_pop(L, 1); // factory bp
            }
            lua_pop(L, 1); // __blueprints
            lua_pop(L, 1); // factory table
        }

        lua_pop(L, 1); // sub

        if (!found) {
            lua_pushboolean(L, 0);
            return 1;
        }
    }

    // All template entries satisfied — return the factories table
    lua_pushvalue(L, facs);
    return 1;
}
```

- [ ] **Step 3: Register in `aibrain_methods[]` table**

Add this entry to the `aibrain_methods[]` array, before the `{nullptr, nullptr}` sentinel:

```cpp
    {"CanBuildPlatoon",         brain_CanBuildPlatoon},
```

- [ ] **Step 4: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add brain:CanBuildPlatoon for factory builder manager

Checks if factory units can build all blueprints in a platoon
template. Parses factory blueprint Economy.BuildableCategory
(table of space-separated category strings) and matches against
each template entry's CategoriesHash. Returns factories table
on success (truthy), false on failure.

Called by FactoryBuilderManager.BuilderParamCheck to validate
factory capabilities before issuing build orders.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: BuildPlatoon

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (insert function after `brain_CanBuildPlatoon`, add entry to `aibrain_methods[]`)

**Reference:**
- `brain_BuildUnit` (line ~5419) shows the pattern: extract Unit* via `_c_object`, create `UnitCommand{BuildFactory, bp_id}`, call `push_command(cmd, false)`
- Template sub-table format: `{bp_id, min, max, squad, formation}` — use `sub[1]` for bp_id
- In the common skirmish case, factories is `{factory}` (single element) and template has a single sub-table

- [ ] **Step 1: Add `brain_BuildPlatoon` function**

Insert after `brain_CanBuildPlatoon`:

```cpp
// brain:BuildPlatoon(template, factories, count)
// template = {name, plan, {bp_id, min, max, squad, formation}, ...}
// factories = {factory1, ...}
// count = multiplier for how many of each unit to build
// Returns: nil (fire-and-forget)
static int brain_BuildPlatoon(lua_State* L) {
    auto* brain = check_brain(L);
    if (!brain || !lua_istable(L, 2) || !lua_istable(L, 3)) return 0;

    int tmpl = 2;
    int facs = 3;
    int count = lua_isnumber(L, 4) ? static_cast<int>(lua_tonumber(L, 4)) : 1;
    if (count < 1) count = 1;

    // Collect factory Unit* pointers
    std::vector<sim::Unit*> factories;
    for (int f = 1; ; f++) {
        lua_rawgeti(L, facs, f);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        auto* e = lua_isuserdata(L, -1)
                      ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                      : nullptr;
        lua_pop(L, 1); // _c_object
        lua_pop(L, 1); // factory table
        if (e && e->is_unit() && !e->destroyed())
            factories.push_back(static_cast<sim::Unit*>(e));
    }

    if (factories.empty()) return 0;

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = blueprint ID
        lua_rawgeti(L, sub, 1);
        if (!lua_isstring(L, -1)) { lua_pop(L, 2); continue; }
        std::string bp_id = lua_tostring(L, -1);
        lua_pop(L, 1);

        // sub[2] = min count
        lua_rawgeti(L, sub, 2);
        int num = lua_isnumber(L, -1)
                      ? static_cast<int>(lua_tonumber(L, -1)) * count
                      : count;
        lua_pop(L, 1);

        lua_pop(L, 1); // sub

        // Distribute build commands across factories (round-robin)
        auto* factory = factories[(i - 3) % factories.size()];
        for (int n = 0; n < num; n++) {
            sim::UnitCommand cmd;
            cmd.type = sim::CommandType::BuildFactory;
            cmd.blueprint_id = bp_id;
            factory->push_command(cmd, false);
        }
    }

    return 0;
}
```

- [ ] **Step 2: Register in `aibrain_methods[]` table**

Add this entry to the `aibrain_methods[]` array, before `{nullptr, nullptr}`:

```cpp
    {"BuildPlatoon",            brain_BuildPlatoon},
```

- [ ] **Step 3: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add brain:BuildPlatoon for factory production queuing

Parses platoon template sub-tables for blueprint IDs, extracts
factory Unit* from factories table, issues BuildFactory commands
via push_command. Uses same pattern as brain:BuildUnit.

Called by FactoryBuilderManager.AssignBuildOrder to queue unit
production on factories.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Chunk 3: Platoon Form Methods

### Task 5: CanFormPlatoon

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (insert function after `platoon_CalculatePlatoonThreatAroundPosition`, add entry to `platoon_methods[]`)

**Reference:**
- Form template format: `{name, plan, {category_expr, min, max, squad, formation}, ...}`
- `sub[1]` is a Lua category expression table (NOT a string) — use `unit_matches_category(L, cat_idx, unit->categories())`
- `platoon_CalculatePlatoonThreat` (line ~6198) shows pattern for iterating `platoon->unit_ids()` with category filtering
- Position/radius filtering follows `brain_GetNumUnitsAroundPoint` pattern (2D distance check)

- [ ] **Step 1: Add `platoon_CanFormPlatoon` function**

Insert after `platoon_CalculatePlatoonThreatAroundPosition`:

```cpp
// platoon:CanFormPlatoon(template, count, location, radius)
// template = {name, plan, {category_expr, min, max, squad, formation}, ...}
// count = multiplier for min/max counts
// location = optional {x, y, z} position
// radius = optional search radius
// Returns: boolean
static int platoon_CanFormPlatoon(lua_State* L) {
    auto* platoon = check_platoon(L);
    auto* sim = get_sim(L);
    if (!platoon || !sim || !lua_istable(L, 2)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int tmpl = 2;
    int multiplier = lua_isnumber(L, 3)
                         ? static_cast<int>(lua_tonumber(L, 3))
                         : 1;
    if (multiplier < 1) multiplier = 1;

    // Optional location + radius filter
    bool has_location = lua_istable(L, 4) && lua_isnumber(L, 5);
    f32 loc_x = 0, loc_z = 0, radius_sq = 0;
    if (has_location) {
        lua_rawgeti(L, 4, 1);
        loc_x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 3);
        loc_z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        f32 r = static_cast<f32>(lua_tonumber(L, 5));
        radius_sq = r * r;
    }

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = category expression (Lua table)
        lua_rawgeti(L, sub, 1);
        int cat_idx = lua_gettop(L);
        if (!lua_istable(L, cat_idx)) {
            lua_pop(L, 2); // cat + sub
            continue;
        }

        // sub[2] = min count
        lua_rawgeti(L, sub, 2);
        int min_count = lua_isnumber(L, -1)
                            ? static_cast<int>(lua_tonumber(L, -1)) * multiplier
                            : multiplier;
        lua_pop(L, 1);

        // Count matching units in pool
        int matched = 0;
        for (u32 id : platoon->unit_ids()) {
            auto* e = sim->entity_registry().find(id);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<sim::Unit*>(e);

            if (!osc::lua::unit_matches_category(L, cat_idx, unit->categories()))
                continue;

            if (has_location) {
                auto pos = unit->position();
                f32 dx = pos.x - loc_x;
                f32 dz = pos.z - loc_z;
                if (dx * dx + dz * dz > radius_sq) continue;
            }

            matched++;
        }

        lua_pop(L, 1); // cat_idx
        lua_pop(L, 1); // sub

        if (matched < min_count) {
            lua_pushboolean(L, 0);
            return 1;
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}
```

- [ ] **Step 2: Register in `platoon_methods[]` table**

Add this entry to the `platoon_methods[]` array, before `{nullptr, nullptr}`:

```cpp
    {"CanFormPlatoon",          platoon_CanFormPlatoon},
```

- [ ] **Step 3: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add platoon:CanFormPlatoon for platoon form manager

Checks if pool platoon has enough units matching each template
entry's category expression. Uses unit_matches_category() for
compound category trees (union/intersection/difference). Optional
position/radius filter for spatial queries.

Called by PlatoonFormManager.ManagerLoopBody to check if idle
units can form a squad before calling FormPlatoon.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: FormPlatoon

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (insert function after `platoon_CanFormPlatoon`, add entry to `platoon_methods[]`)

**Reference:**
- `brain_MakePlatoon` (line ~5489) shows the full pattern for creating a platoon Lua table with `_c_object`, metatable, `lua_table_ref`, and `OnCreate` callback
- Must snapshot `unit_ids_` before iterating since `remove_unit()` modifies the vector
- Platoon methods: `add_unit()`, `remove_unit()`, `set_unit_squad()`, `set_plan_name()`
- Brain access: `sim->get_army(platoon->army_index())` to get ArmyBrain for `create_platoon()`

- [ ] **Step 1: Add `platoon_FormPlatoon` function**

Insert after `platoon_CanFormPlatoon`:

```cpp
// platoon:FormPlatoon(template, count, location, radius)
// Same args as CanFormPlatoon. Extracts matching units from pool
// into a new platoon and returns it.
// Returns: new platoon Lua table
static int platoon_FormPlatoon(lua_State* L) {
    auto* pool = check_platoon(L);
    auto* sim = get_sim(L);
    if (!pool || !sim || !lua_istable(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    int tmpl = 2;
    int multiplier = lua_isnumber(L, 3)
                         ? static_cast<int>(lua_tonumber(L, 3))
                         : 1;
    if (multiplier < 1) multiplier = 1;

    // Optional location + radius filter
    bool has_location = lua_istable(L, 4) && lua_isnumber(L, 5);
    f32 loc_x = 0, loc_z = 0, radius_sq = 0;
    if (has_location) {
        lua_rawgeti(L, 4, 1);
        loc_x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 4, 3);
        loc_z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        f32 r = static_cast<f32>(lua_tonumber(L, 5));
        radius_sq = r * r;
    }

    // Get brain to create new platoon
    auto* brain = sim->get_army(pool->army_index());
    if (!brain) { lua_pushnil(L); return 1; }

    // Get plan name from template[2]
    lua_rawgeti(L, tmpl, 2);
    std::string plan = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    auto* new_platoon = brain->create_platoon("");
    new_platoon->set_plan_name(plan);

    // Snapshot pool unit IDs (remove_unit modifies the vector)
    auto pool_ids = pool->unit_ids();

    // Track which IDs we've transferred (to remove from pool after)
    std::vector<u32> transferred;

    // Iterate template sub-tables starting at index 3
    for (int i = 3; ; i++) {
        lua_rawgeti(L, tmpl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int sub = lua_gettop(L);

        // sub[1] = category expression
        lua_rawgeti(L, sub, 1);
        int cat_idx = lua_gettop(L);
        if (!lua_istable(L, cat_idx)) {
            lua_pop(L, 2); // cat + sub
            continue;
        }

        // sub[2] = min, sub[3] = max
        lua_rawgeti(L, sub, 2);
        int min_count = lua_isnumber(L, -1)
                            ? static_cast<int>(lua_tonumber(L, -1)) * multiplier
                            : multiplier;
        lua_pop(L, 1);

        lua_rawgeti(L, sub, 3);
        int max_count = lua_isnumber(L, -1)
                            ? static_cast<int>(lua_tonumber(L, -1)) * multiplier
                            : multiplier;
        lua_pop(L, 1);

        // sub[4] = squad name
        lua_rawgeti(L, sub, 4);
        std::string squad = lua_isstring(L, -1) ? lua_tostring(L, -1) : "Unassigned";
        lua_pop(L, 1);

        // Find matching units and transfer
        int taken = 0;
        for (u32 id : pool_ids) {
            if (taken >= max_count) break;

            // Skip if already transferred in a previous sub-table
            bool already = false;
            for (u32 t : transferred) {
                if (t == id) { already = true; break; }
            }
            if (already) continue;

            auto* e = sim->entity_registry().find(id);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<sim::Unit*>(e);

            if (!osc::lua::unit_matches_category(L, cat_idx, unit->categories()))
                continue;

            if (has_location) {
                auto pos = unit->position();
                f32 dx = pos.x - loc_x;
                f32 dz = pos.z - loc_z;
                if (dx * dx + dz * dz > radius_sq) continue;
            }

            new_platoon->add_unit(id);
            new_platoon->set_unit_squad(id, squad);
            transferred.push_back(id);
            taken++;
        }

        lua_pop(L, 1); // cat_idx
        lua_pop(L, 1); // sub
    }

    // Remove transferred units from pool
    for (u32 id : transferred) {
        pool->remove_unit(id);
    }

    // Build Lua table for new platoon (same pattern as brain_MakePlatoon)
    lua_newtable(L);
    int plat_tbl = lua_gettop(L);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, new_platoon);
    lua_rawset(L, plat_tbl);

    lua_pushstring(L, "PlanName");
    lua_pushstring(L, plan.c_str());
    lua_rawset(L, plat_tbl);

    // Set metatable (cached __osc_platoon_mt or __platoon_class)
    lua_pushstring(L, "__platoon_class");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "__osc_platoon_mt");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            // Create cached metatable
            lua_newtable(L);
            lua_pushstring(L, "__index");
            lua_pushstring(L, "moho");
            lua_rawget(L, LUA_GLOBALSINDEX);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "platoon_methods");
                lua_rawget(L, -2);
                lua_remove(L, -2);
            } else {
                lua_pop(L, 1);
                lua_pushnil(L);
            }
            lua_settable(L, -3);
            lua_pushstring(L, "__osc_platoon_mt");
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
    }
    lua_setmetatable(L, plat_tbl);

    // Store lua_table_ref on platoon
    lua_pushvalue(L, plat_tbl);
    new_platoon->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // Call OnCreate if available
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, plat_tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, plat_tbl);
        lua_pushstring(L, plan.c_str());
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("FormPlatoon OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::info("FormPlatoon: id={} army={} plan='{}' units={}",
                 new_platoon->platoon_id(), brain->index(), plan,
                 new_platoon->unit_ids().size());

    // Return the platoon table (defensive push matching brain_MakePlatoon pattern)
    lua_pushvalue(L, plat_tbl);
    return 1;
}
```

- [ ] **Step 2: Register in `platoon_methods[]` table**

Add this entry to the `platoon_methods[]` array, before `{nullptr, nullptr}`:

```cpp
    {"FormPlatoon",             platoon_FormPlatoon},
```

- [ ] **Step 3: Build and run tests**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.

- [ ] **Step 4: Quick integration smoke test**

Run a short AI skirmish to verify no crashes with the new methods:

```bash
./build/Debug/osc_engine.exe --ai-skirmish --ticks 200 --map "//maps/SCMP_009/SCMP_009_scenario.lua" 2>&1 | grep -c "Thread error"
```
Expected: Thread error count should be lower than before (specifically, `CanFormPlatoon` and `GetPlatoonThreat` errors should be gone).

- [ ] **Step 5: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "$(cat <<'EOF'
Add platoon:FormPlatoon for platoon form manager

Extracts units from pool platoon into a new platoon based on
template category expressions. For each template entry, walks
pool units, matches via unit_matches_category(), transfers up
to max*count matching units. Creates Lua table with _c_object
lightuserdata, cached metatable, and OnCreate callback — same
pattern as brain:MakePlatoon.

Snapshots unit_ids before iterating to avoid iterator invalidation
from remove_unit() calls.

Called by PlatoonFormManager.ManagerLoopBody after CanFormPlatoon
returns true.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Chunk 4: Integration Validation

### Task 7: Full AI Skirmish Integration Test

**Files:** None (testing only)

**Reference:**
- Spec success criteria: AI builds factory within 1000 ticks, produces unit within 2000 ticks, sends platoon to attack within 3000 ticks
- Thread errors for `CanFormPlatoon` and `GetPlatoonThreat` should be eliminated

- [ ] **Step 1: Run full AI skirmish validation**

Run:
```bash
./build/Debug/osc_engine.exe --ai-skirmish --ticks 3000 --map "//maps/SCMP_009/SCMP_009_scenario.lua" 2>&1 | tee /tmp/ai_skirmish.log
```

Check the log for:
```bash
# Verify CanFormPlatoon/GetPlatoonThreat errors are gone
grep -c "CanFormPlatoon" /tmp/ai_skirmish.log
grep -c "GetPlatoonThreat" /tmp/ai_skirmish.log

# Check for factory construction
grep -i "factory" /tmp/ai_skirmish.log | head -5

# Check for unit production
grep -i "BuildPlatoon\|BuildFactory\|FactoryFinish" /tmp/ai_skirmish.log | head -5

# Check for platoon formation
grep -i "FormPlatoon\|CanFormPlatoon" /tmp/ai_skirmish.log | head -5

# Check for remaining thread errors
grep "Thread error" /tmp/ai_skirmish.log | sort -u
```

Expected:
- `CanFormPlatoon` and `GetPlatoonThreat` thread error count = 0
- Evidence of factory construction and unit production in logs
- Any remaining thread errors are non-blocking (not in the builder chain)

- [ ] **Step 2: Triage remaining thread errors**

If new thread errors appear for methods not yet implemented, document them for Phase 3/4. Common expected ones:
- `aipersonality_methods` related (Phase 3 scope)
- `SetupCheat` / buff system (Phase 3 scope)
- Visual/cosmetic methods (Phase 4 / skip)

- [ ] **Step 3: Final build + full test suite**

Run:
```bash
cmake --build build --config Debug 2>&1 | tail -5 && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -3
```
Expected: Build succeeds, all tests pass.
