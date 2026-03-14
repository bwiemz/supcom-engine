# M153-M154: FA Lua Smoke Test & Quick-Fix Triage — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load FA's real Lua codebase through the engine, produce a categorized failure report, and fix all trivially-solvable issues.

**Architecture:** A `SmokeTestHarness` class intercepts missing globals and missing moho methods via Lua `__index` metamethods, collects structured log entries, and outputs a deduplicated report. A `--smoke-test` CLI flag wires it into the existing boot sequence. M154 fixes are isolated changes to `sim_bindings.cpp` and `sim_state.hpp`.

**Tech Stack:** C++17, Lua 5.0, Catch2 v3, spdlog

**Spec:** `docs/plans/2026-03-13-m153-m163-playable-skirmish-design.md` (Phase 1)

---

## File Structure

| Action | File | Responsibility |
|--------|------|---------------|
| Create | `src/lua/smoke_test.hpp` | SmokeTestHarness class declaration |
| Create | `src/lua/smoke_test.cpp` | SmokeTestHarness implementation (interceptors, reporting) |
| Create | `tests/test_smoke_test.cpp` | Unit tests for smoke test logger and interceptors |
| Modify | `tests/CMakeLists.txt:1-17` | Add test_smoke_test.cpp to test executable |
| Modify | `src/main.cpp:363-467` | Add `--smoke-test` CLI flag parsing |
| Modify | `src/main.cpp:1234-1243` | Add smoke test headless boot path |
| Modify | `src/lua/sim_bindings.cpp:4092` | Wire `IsGameOver` to `player_result()` |
| Modify | `src/lua/sim_bindings.cpp:4123,4346` | Implement `SetPlayableRect` |
| Modify | `src/lua/sim_bindings.cpp:4228-4229` | Implement `AddBuildRestriction`/`RemoveBuildRestriction` |
| Modify | `src/lua/sim_bindings.cpp:4220` | Implement `ArmyInitializePrebuiltUnits` |
| Modify | `src/sim/sim_state.hpp:193-241` | Add playable rect fields |
| Modify | `src/sim/sim_state.cpp` | Add playable rect accessors |
| Modify | `src/sim/army_brain.hpp:145-172` | Add build restriction set |
| Modify | `src/sim/navigator.hpp` | Add SimState pointer for playable rect clamping |
| Modify | `src/sim/navigator.cpp:53-134` | Clamp movement to playable rect |

---

## Chunk 1: SmokeTestHarness Infrastructure (M153)

### Task 1: SmokeTestHarness — Log Entry and Reporter

**Files:**
- Create: `src/lua/smoke_test.hpp`
- Create: `src/lua/smoke_test.cpp`
- Create: `tests/test_smoke_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test for SmokeTestHarness log collection**

File: `tests/test_smoke_test.cpp`
```cpp
#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"

TEST_CASE("SmokeTestHarness records and deduplicates entries", "[smoke]") {
    osc::lua::SmokeTestHarness harness;

    harness.record(osc::lua::SmokeCategory::MissingGlobal, "FooFunc", "test.lua:10");
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "FooFunc", "test.lua:20");
    harness.record(osc::lua::SmokeCategory::MissingMethod, "unit.BarMethod", "unit.lua:5");
    harness.record(osc::lua::SmokeCategory::PcallError, "attempt to index nil", "sim.lua:99");

    auto report = harness.generate_report();

    // 3 unique entries (FooFunc deduplicated)
    REQUIRE(report.size() == 3);

    // Find the FooFunc entry — count should be 2
    bool found_foo = false;
    for (auto& e : report) {
        if (e.name == "FooFunc") {
            REQUIRE(e.category == osc::lua::SmokeCategory::MissingGlobal);
            REQUIRE(e.count == 2);
            REQUIRE(e.first_location == "test.lua:10");
            found_foo = true;
        }
    }
    REQUIRE(found_foo);
}

TEST_CASE("SmokeTestHarness total_count sums all occurrences", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "A", "a.lua:1");
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "A", "a.lua:2");
    harness.record(osc::lua::SmokeCategory::MissingMethod, "B", "b.lua:1");
    REQUIRE(harness.total_count() == 3);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: FAIL — `smoke_test.hpp` not found

- [ ] **Step 3: Add test file to CMakeLists.txt**

File: `tests/CMakeLists.txt` — add `test_smoke_test.cpp` to the `add_executable(osc_tests ...)` list, after `test_phase1.cpp`.

- [ ] **Step 4: Write SmokeTestHarness header**

File: `src/lua/smoke_test.hpp`
```cpp
#pragma once

#include "core/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::lua {

enum class SmokeCategory {
    MissingGlobal,
    MissingMethod,
    PcallError,
    WrongReturn, // Deferred: detection requires per-function return type knowledge.
                 // Category exists for manual tagging during triage (Task 11).
};

struct SmokeReportEntry {
    SmokeCategory category;
    std::string name;
    std::string first_location;
    u32 count;
};

class SmokeTestHarness {
public:
    void record(SmokeCategory category, const std::string& name,
                const std::string& location);

    std::vector<SmokeReportEntry> generate_report() const;
    u32 total_count() const;

    void print_report() const;

private:
    struct EntryKey {
        SmokeCategory category;
        std::string name;
        bool operator==(const EntryKey& o) const {
            return category == o.category && name == o.name;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey& k) const {
            return std::hash<std::string>()(k.name) ^
                   (std::hash<int>()(static_cast<int>(k.category)) << 16);
        }
    };
    struct EntryData {
        std::string first_location;
        u32 count = 0;
    };
    std::unordered_map<EntryKey, EntryData, EntryKeyHash> entries_;
};

} // namespace osc::lua
```

Note: the header includes `core/types.hpp` which provides `osc::u32`. The forward declaration `struct lua_State;` avoids pulling in the full Lua headers.

- [ ] **Step 5: Write SmokeTestHarness implementation**

File: `src/lua/smoke_test.cpp`
```cpp
#include "lua/smoke_test.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace osc::lua {

void SmokeTestHarness::record(SmokeCategory category, const std::string& name,
                               const std::string& location) {
    EntryKey key{category, name};
    auto& data = entries_[key];
    if (data.count == 0) data.first_location = location;
    data.count++;
}

std::vector<SmokeReportEntry> SmokeTestHarness::generate_report() const {
    std::vector<SmokeReportEntry> result;
    result.reserve(entries_.size());
    for (auto& [key, data] : entries_) {
        result.push_back({key.category, key.name, data.first_location, data.count});
    }
    // Sort by count descending (highest-impact first)
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.count > b.count; });
    return result;
}

u32 SmokeTestHarness::total_count() const {
    u32 total = 0;
    for (auto& [key, data] : entries_) total += data.count;
    return total;
}

void SmokeTestHarness::print_report() const {
    auto report = generate_report();
    spdlog::info("=== Smoke Test Report ({} unique issues, {} total occurrences) ===",
                 report.size(), total_count());
    const char* cat_names[] = {"MISSING_GLOBAL", "MISSING_METHOD", "PCALL_ERROR", "WRONG_RETURN"};
    for (auto& e : report) {
        spdlog::info("  [{:15s}] {:40s} x{:4d}  (first: {})",
                     cat_names[static_cast<int>(e.category)], e.name, e.count, e.first_location);
    }
}

} // namespace osc::lua
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: PASS — 2 tests, all assertions pass

- [ ] **Step 7: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_test.cpp tests/CMakeLists.txt
git commit -m "M153a: Add SmokeTestHarness log collection and reporting"
```

---

### Task 2: Missing Global Interceptor

**Files:**
- Modify: `src/lua/smoke_test.hpp`
- Modify: `src/lua/smoke_test.cpp`
- Modify: `tests/test_smoke_test.cpp`

- [ ] **Step 1: Write the failing test for global interceptor**

Add to `tests/test_smoke_test.cpp`:
```cpp
#include "lua/lua_state.hpp"

TEST_CASE("SmokeTestHarness intercepts missing globals", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_global_interceptor(state.raw());

    // Access a global that doesn't exist — should be logged, return nil
    auto result = state.do_string("local x = SomeMissingGlobal\nreturn type(x)");
    REQUIRE(result.ok());

    // Check that the missing access was recorded
    auto report = harness.generate_report();
    bool found = false;
    for (auto& e : report) {
        if (e.name == "SomeMissingGlobal" &&
            e.category == osc::lua::SmokeCategory::MissingGlobal) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("Global interceptor does not log existing globals", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;

    // Set a global first
    state.do_string("MyGlobal = 42");
    harness.install_global_interceptor(state.raw());

    state.do_string("local x = MyGlobal");
    REQUIRE(harness.total_count() == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: FAIL — `install_global_interceptor` not declared

- [ ] **Step 3: Add install_global_interceptor declaration to header**

In `src/lua/smoke_test.hpp`, add to the `SmokeTestHarness` class public section:
```cpp
    /// Install __index metamethod on the globals table that logs missing accesses.
    /// The harness pointer is stored in the Lua registry as "__osc_smoke_harness".
    void install_global_interceptor(lua_State* L);
```

Add at the top of the file:
```cpp
struct lua_State;
```

- [ ] **Step 4: Write install_global_interceptor implementation**

In `src/lua/smoke_test.cpp`, add the following:

```cpp
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// C callback for globals __index metamethod
static int smoke_global_index(lua_State* L) {
    // Stack: globals_table, key
    const char* key = lua_tostring(L, 2);
    if (!key) {
        lua_pushnil(L);
        return 1;
    }

    // __index is only called when the key is NOT found via raw lookup in the
    // globals table, so we know it's missing — just log and return nil.
    lua_pushstring(L, "__osc_smoke_harness");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* harness = static_cast<osc::lua::SmokeTestHarness*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (harness) {
        // Build location string from debug info
        lua_Debug ar;
        std::string location = "?";
        if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
            location = std::string(ar.source ? ar.source : "?") + ":" + std::to_string(ar.currentline);
        }
        harness->record(osc::lua::SmokeCategory::MissingGlobal, key, location);
    }

    lua_pushnil(L);
    return 1;
}

void osc::lua::SmokeTestHarness::install_global_interceptor(lua_State* L) {
    // Store harness pointer in registry
    lua_pushstring(L, "__osc_smoke_harness");
    lua_pushlightuserdata(L, this);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Create metatable for globals with __index handler
    lua_newtable(L);                        // metatable
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, smoke_global_index);
    lua_rawset(L, -3);                      // mt.__index = smoke_global_index

    // Set as metatable of globals table
    // In Lua 5.0: lua_pushvalue(L, LUA_GLOBALSINDEX) then lua_setmetatable
    lua_setmetatable(L, LUA_GLOBALSINDEX);
}
```

**Key Lua 5.0 detail:** `LUA_GLOBALSINDEX` is a pseudo-index. `lua_setmetatable(L, LUA_GLOBALSINDEX)` sets the metatable on the globals table itself. The `__index` metamethod fires when a key is NOT found as a raw value in the table — so existing globals (including all registered C functions) will not trigger it.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: PASS — all 4 smoke tests pass

- [ ] **Step 6: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_test.cpp
git commit -m "M153b: Add global interceptor for smoke test missing-global detection"
```

---

### Task 3: Missing Method Interceptor

**Files:**
- Modify: `src/lua/smoke_test.hpp`
- Modify: `src/lua/smoke_test.cpp`
- Modify: `tests/test_smoke_test.cpp`

- [ ] **Step 1: Write the failing test for method interceptor**

Add to `tests/test_smoke_test.cpp`:
```cpp
TEST_CASE("SmokeTestHarness intercepts missing moho methods", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;

    // Create a fake moho metatable (simulating the cached __osc_*_mt pattern)
    lua_State* L = state.raw();

    // Create a metatable and register it as __osc_test_mt
    lua_newtable(L);
    int mt = lua_gettop(L);
    lua_pushstring(L, "__index");
    lua_pushvalue(L, mt);
    lua_rawset(L, mt); // mt.__index = mt

    // Add one real method
    lua_pushstring(L, "RealMethod");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        lua_pushnumber(L, 42);
        return 1;
    });
    lua_rawset(L, mt);

    // Cache it
    lua_pushstring(L, "__osc_test_mt");
    lua_pushvalue(L, mt);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1); // pop mt

    // Install method interceptor on this metatable
    harness.install_method_interceptor(L, "__osc_test_mt", "TestObj");

    // Create an object with this metatable
    lua_newtable(L);
    lua_pushstring(L, "__osc_test_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    lua_setglobal(L, "test_obj");

    // Call a real method — should work, no log
    state.do_string("local r = test_obj:RealMethod()");
    REQUIRE(harness.total_count() == 0);

    // Call a missing method — should log, return no-op function
    state.do_string("test_obj:FakeMethod()");
    auto report = harness.generate_report();
    REQUIRE(report.size() == 1);
    REQUIRE(report[0].name == "TestObj.FakeMethod");
    REQUIRE(report[0].category == osc::lua::SmokeCategory::MissingMethod);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: FAIL — `install_method_interceptor` not declared

- [ ] **Step 3: Add install_method_interceptor declaration**

In `src/lua/smoke_test.hpp`, add to the `SmokeTestHarness` class public section:
```cpp
    /// Install __index fallback on a cached metatable to log missing method calls.
    /// registry_key: e.g., "__osc_proj_mt". type_name: e.g., "Projectile".
    void install_method_interceptor(lua_State* L, const char* registry_key,
                                     const char* type_name);

    /// Install method interceptors on all known __osc_*_mt metatables.
    void install_all_method_interceptors(lua_State* L);
```

- [ ] **Step 4: Write implementation**

In `src/lua/smoke_test.cpp`, add:

```cpp
// Upvalue layout for method interceptor __index:
// upvalue 1 = original __index (the metatable itself)
// upvalue 2 = type_name string
// upvalue 3 = harness lightuserdata
static int smoke_method_index(lua_State* L) {
    // Stack: object, key
    // First try the original metatable
    lua_pushvalue(L, 2);                    // push key
    lua_rawget(L, lua_upvalueindex(1));     // rawget from original metatable
    if (!lua_isnil(L, -1)) {
        return 1; // Found — return it
    }
    lua_pop(L, 1);

    const char* key = lua_tostring(L, 2);
    if (!key) {
        lua_pushnil(L);
        return 1;
    }

    // Log the missing method
    auto* harness = static_cast<osc::lua::SmokeTestHarness*>(
        lua_touserdata(L, lua_upvalueindex(3)));
    const char* type_name = lua_tostring(L, lua_upvalueindex(2));

    if (harness && type_name) {
        std::string full_name = std::string(type_name) + "." + key;
        lua_Debug ar;
        std::string location = "?";
        if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
            location = std::string(ar.source ? ar.source : "?") + ":" + std::to_string(ar.currentline);
        }
        harness->record(osc::lua::SmokeCategory::MissingMethod, full_name, location);
    }

    // Return a no-op function so the call doesn't error
    lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
    return 1;
}

void osc::lua::SmokeTestHarness::install_method_interceptor(
    lua_State* L, const char* registry_key, const char* type_name) {
    // Get the cached metatable
    lua_pushstring(L, registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return; // Metatable not yet created — skip
    }
    int mt = lua_gettop(L);

    // Replace __index with our interceptor closure
    // The closure captures: (1) original metatable, (2) type_name, (3) harness ptr
    lua_pushstring(L, "__index");
    lua_pushvalue(L, mt);                          // upvalue 1: original mt
    lua_pushstring(L, type_name);                  // upvalue 2: type name
    lua_pushlightuserdata(L, this);                // upvalue 3: harness
    lua_pushcclosure(L, smoke_method_index, 3);
    lua_rawset(L, mt);  // mt.__index = closure

    lua_pop(L, 1); // pop mt
}

void osc::lua::SmokeTestHarness::install_all_method_interceptors(lua_State* L) {
    struct MtEntry { const char* key; const char* name; };
    static const MtEntry entries[] = {
        {"__osc_proj_mt",      "Projectile"},
        {"__osc_nav_mt",       "Navigator"},
        {"__osc_blip_mt",      "Blip"},
        {"__osc_weapon_mt",    "Weapon"},
        {"__osc_platoon_mt",   "Platoon"},
        {"__osc_ui_unit_mt",   "UIUnit"},
        {"__osc_ieffect_mt",   "IEffect"},
        {"__osc_rotate_mt",    "RotateManipulator"},
        {"__osc_anim_mt",      "AnimManipulator"},
        {"__osc_slide_mt",     "SlideManipulator"},
        {"__osc_aim_mt",       "AimManipulator"},
        {"__osc_coldet_mt",    "CollisionDetector"},
        {"__osc_footplant_mt", "FootPlant"},
        {"__osc_slaver_mt",    "Slaver"},
        {"__osc_storage_mt",   "Storage"},
        {"__osc_thrust_mt",    "Thrust"},
        {"__osc_vector_mt",    "Vector"},
        {"__osc_thread_mt",    "Thread"},
    };
    for (auto& e : entries) {
        install_method_interceptor(L, e.key, e.name);
    }

    // Also intercept unit_methods and entity_methods via the moho table
    // These use a different pattern: moho.unit_methods is the __index target
    // We install on the moho.unit_methods table directly
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        struct MohoEntry { const char* field; const char* name; };
        static const MohoEntry moho_entries[] = {
            {"unit_methods",     "Unit"},
            {"entity_methods",   "Entity"},
            {"army_methods",     "ArmyBrain"},
        };
        int moho_idx = lua_gettop(L);
        for (auto& me : moho_entries) {
            lua_pushstring(L, me.field);
            lua_rawget(L, moho_idx);
            if (lua_istable(L, -1)) {
                // This table IS the __index target. We need to set a metatable on IT
                // with an __index handler for missing keys.
                lua_newtable(L); // meta-metatable
                lua_pushstring(L, "__index");
                // Closure with: (1) dummy table (unused), (2) type_name, (3) harness
                lua_newtable(L);                      // upvalue 1 (unused)
                lua_pushstring(L, me.name);           // upvalue 2
                lua_pushlightuserdata(L, this);        // upvalue 3
                lua_pushcclosure(L, smoke_method_index, 3);
                lua_rawset(L, -3); // meta_mt.__index = closure
                lua_setmetatable(L, -2); // set meta-metatable on moho.X_methods
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // pop moho
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: PASS — all smoke tests pass

- [ ] **Step 6: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_test.cpp
git commit -m "M153c: Add method interceptor for smoke test missing-method detection"
```

---

### Task 4: Panic Handler and Pcall Error Wrapper

**Files:**
- Modify: `src/lua/smoke_test.hpp`
- Modify: `src/lua/smoke_test.cpp`
- Modify: `tests/test_smoke_test.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_smoke_test.cpp`:
```cpp
TEST_CASE("SmokeTestHarness panic handler prevents abort", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_panic_handler(state.raw());

    // A pcall error should be caught by our error handler, not crash
    auto result = state.do_string("error('test error')");
    // do_string uses lua_pcall internally, so it should return an error
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("SmokeTestHarness pcall error recording", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_panic_handler(state.raw());

    // Run code that will error
    harness.do_string_logged(state.raw(), "local x = nil; x.foo()");

    auto report = harness.generate_report();
    REQUIRE(report.size() >= 1);
    bool found_pcall = false;
    for (auto& e : report) {
        if (e.category == osc::lua::SmokeCategory::PcallError) found_pcall = true;
    }
    REQUIRE(found_pcall);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: FAIL — `install_panic_handler` / `do_string_logged` not declared

- [ ] **Step 3: Add declarations to header**

In `src/lua/smoke_test.hpp`, add to the `SmokeTestHarness` class:
```cpp
    /// Install lua_atpanic handler that logs instead of aborting.
    void install_panic_handler(lua_State* L);

    /// Execute a Lua string, recording any pcall errors in the harness.
    bool do_string_logged(lua_State* L, const char* code);

    /// Execute a Lua file (via VFS buffer), recording any pcall errors.
    bool do_buffer_logged(lua_State* L, const char* buffer, size_t len,
                          const char* name);
```

- [ ] **Step 4: Write implementation**

In `src/lua/smoke_test.cpp`:
```cpp
static int smoke_panic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    spdlog::error("SMOKE PANIC (unrecoverable): {}", msg ? msg : "(no message)");
    // Note: In Lua 5.0, lua_atpanic fires only when there is NO active lua_pcall.
    // After this handler returns, Lua calls exit(EXIT_FAILURE).
    // Do NOT access Lua registry here — state may be corrupted.
    // All recoverable errors are caught by do_string_logged/do_buffer_logged via pcall.
    return 0;
}

void osc::lua::SmokeTestHarness::install_panic_handler(lua_State* L) {
    // Store harness in registry (may already be set by install_global_interceptor)
    lua_pushstring(L, "__osc_smoke_harness");
    lua_pushlightuserdata(L, this);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_atpanic(L, smoke_panic);
}

bool osc::lua::SmokeTestHarness::do_string_logged(lua_State* L, const char* code) {
    int status = luaL_loadstring(L, code);
    if (status != 0) {
        const char* err = lua_tostring(L, -1);
        record(SmokeCategory::PcallError, err ? err : "(load error)", "do_string");
        lua_pop(L, 1);
        return false;
    }
    status = lua_pcall(L, 0, 0, 0);
    if (status != 0) {
        const char* err = lua_tostring(L, -1);
        record(SmokeCategory::PcallError, err ? err : "(runtime error)", "do_string");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool osc::lua::SmokeTestHarness::do_buffer_logged(lua_State* L, const char* buffer,
                                                    size_t len, const char* name) {
    int status = luaL_loadbuffer(L, buffer, len, name);
    if (status != 0) {
        const char* err = lua_tostring(L, -1);
        record(SmokeCategory::PcallError, err ? err : "(load error)", name);
        lua_pop(L, 1);
        return false;
    }
    status = lua_pcall(L, 0, 0, 0);
    if (status != 0) {
        const char* err = lua_tostring(L, -1);
        record(SmokeCategory::PcallError, err ? err : "(runtime error)", name);
        lua_pop(L, 1);
        return false;
    }
    return true;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[smoke]" -v`
Expected: PASS — all smoke tests pass

- [ ] **Step 6: Commit**

```bash
git add src/lua/smoke_test.hpp src/lua/smoke_test.cpp tests/test_smoke_test.cpp
git commit -m "M153d: Add panic handler and logged pcall for smoke test error recording"
```

---

### Task 5: Wire --smoke-test CLI Flag

**Files:**
- Modify: `src/main.cpp`

This task does not have a unit test — it is an integration-level feature tested by running the engine with `--smoke-test`. The SmokeTestHarness itself is unit-tested in Tasks 1-4.

- [ ] **Step 1: Add --smoke-test flag parsing**

In `src/main.cpp`, near lines 363-461 where other test flags are parsed, add:
```cpp
bool smoke_test = parse_flag(argc, argv, "--smoke-test");
```

Also add `smoke_test` to the headless check at line 509:
```cpp
bool headless = tick_count > 0 || damage_test || ... || smoke_test;
```

- [ ] **Step 2: Add smoke_test include**

At the top of `src/main.cpp`, add:
```cpp
#include "lua/smoke_test.hpp"
```

- [ ] **Step 3: Add smoke test boot path**

**Placement note:** The smoke test code runs AFTER the complete boot sequence (sim Lua state, VFS, blueprints, SimState, scenario loading, UI Lua state, UI boot). It must be placed where `sim_lua_state`, `sim_state`, and `ui_lua_state` are all in scope and fully initialized. In main.cpp, this is near the headless tick loop (around line 1234) — after all Phase 1-5 initialization. The `headless` flag is true because `smoke_test` is included in the headless condition.

Add a `smoke_test` path **before** the existing headless block:

```cpp
if (smoke_test) {
    osc::lua::SmokeTestHarness harness;

    // Install interceptors on sim state
    harness.install_panic_handler(sim_lua_state.raw());
    harness.install_global_interceptor(sim_lua_state.raw());
    harness.install_all_method_interceptors(sim_lua_state.raw());

    // Install interceptors on UI state
    harness.install_panic_handler(ui_lua_state.raw());
    harness.install_global_interceptor(ui_lua_state.raw());
    harness.install_all_method_interceptors(ui_lua_state.raw());

    spdlog::info("=== Smoke Test: Running 100 sim ticks ===");
    for (int i = 0; i < 100; i++) {
        sim_state.tick();
    }

    spdlog::info("=== Smoke Test: Running 100 UI frame dispatches ===");
    auto* ui_tm_ptr = [&]() -> osc::sim::ThreadManager* {
        lua_pushstring(ui_lua_state.raw(), "__osc_ui_thread_manager");
        lua_rawget(ui_lua_state.raw(), LUA_REGISTRYINDEX);
        auto* p = static_cast<osc::sim::ThreadManager*>(lua_touserdata(ui_lua_state.raw(), -1));
        lua_pop(ui_lua_state.raw(), 1);
        return p;
    }();
    for (int i = 0; i < 100; i++) {
        if (ui_tm_ptr) ui_tm_ptr->resume_threads(0.1);
    }

    harness.print_report();
    spdlog::info("=== Smoke Test Complete ===");
    return 0;
}
```

- [ ] **Step 4: Build and verify compilation**

Run: `cmake --build build --config Debug`
Expected: Clean build, no errors

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "M153e: Wire --smoke-test CLI flag with interceptors and 100-tick/100-frame run"
```

---

## Chunk 2: M154 Quick-Fix Triage

### Task 6: Wire IsGameOver to player_result()

**Files:**
- Modify: `src/lua/sim_bindings.cpp:4092`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_smoke_test.cpp`:
```cpp
TEST_CASE("IsGameOver returns false for in-progress game", "[m154]") {
    // This tests the function signature; full integration tested via --smoke-test
    osc::lua::LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::lua::register_sim_bindings(state, sim);

    auto result = state.do_string("return IsGameOver()");
    REQUIRE(result.ok());
    // player_result() returns 0 for in-progress -> IsGameOver returns false
    REQUIRE(lua_isboolean(state.raw(), -1));
    REQUIRE(lua_toboolean(state.raw(), -1) == 0);
}
```

**Important:** `register_sim_bindings` stores the SimState pointer in the registry as `"osc_sim_state"`. However, the test must verify that `get_sim(L)` can actually retrieve it. If constructing a full SimState in a unit test is impractical (it may require terrain, pathfinding, etc.), skip this unit test and verify `IsGameOver` via the integration-level smoke test instead. The key verification is that `IsGameOver` returns false for in-progress and true after game-over — the smoke test triage (Task 11) will catch regressions.

- [ ] **Step 2: Implement IsGameOver**

In `src/lua/sim_bindings.cpp`, replace line 4092:
```cpp
// Before:
state.register_function("IsGameOver", stub_false);

// After:
state.register_function("IsGameOver", [](lua_State* L) -> int {
    auto* sim = get_sim(L);
    lua_pushboolean(L, sim && sim->player_result() != 0);
    return 1;
});
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 4: Run existing tests to verify no regression**

Run: `./build/tests/Debug/osc_tests.exe -v`
Expected: All existing tests still pass

- [ ] **Step 5: Commit**

```bash
git add src/lua/sim_bindings.cpp
git commit -m "M154a: Wire IsGameOver() to player_result() — returns true when game ends"
```

---

### Task 7: Implement SetPlayableRect

**Files:**
- Modify: `src/sim/sim_state.hpp:193-241`
- Modify: `src/sim/sim_state.cpp`
- Modify: `src/lua/sim_bindings.cpp:4123,4346`
- Modify: `src/sim/navigator.cpp:53-134`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_smoke_test.cpp`:
```cpp
TEST_CASE("SimState playable rect stores and returns bounds", "[m154]") {
    osc::lua::LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);

    // Default: not set (full map)
    REQUIRE_FALSE(sim.has_playable_rect());

    sim.set_playable_rect(10.0f, 20.0f, 500.0f, 480.0f);
    REQUIRE(sim.has_playable_rect());
    REQUIRE(sim.playable_x0() == 10.0f);
    REQUIRE(sim.playable_z0() == 20.0f);
    REQUIRE(sim.playable_x1() == 500.0f);
    REQUIRE(sim.playable_z1() == 480.0f);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m154]" -v`
Expected: FAIL — `has_playable_rect()` not declared

- [ ] **Step 3: Add playable rect fields to SimState**

In `src/sim/sim_state.hpp`, add to the public section (after `clear_death_events()`, ~line 191):
```cpp
    // Playable area bounds (set by SetPlayableRect Lua call)
    void set_playable_rect(f32 x0, f32 z0, f32 x1, f32 z1) {
        playable_x0_ = x0; playable_z0_ = z0;
        playable_x1_ = x1; playable_z1_ = z1;
        has_playable_rect_ = true;
    }
    bool has_playable_rect() const { return has_playable_rect_; }
    f32 playable_x0() const { return playable_x0_; }
    f32 playable_z0() const { return playable_z0_; }
    f32 playable_x1() const { return playable_x1_; }
    f32 playable_z1() const { return playable_z1_; }

    Vector3 clamp_to_playable(const Vector3& pos) const {
        if (!has_playable_rect_) return pos;
        Vector3 clamped = pos;
        if (clamped.x < playable_x0_) clamped.x = playable_x0_;
        if (clamped.x > playable_x1_) clamped.x = playable_x1_;
        if (clamped.z < playable_z0_) clamped.z = playable_z0_;
        if (clamped.z > playable_z1_) clamped.z = playable_z1_;
        return clamped;
    }
```

In the private section (after `build_ghost_foot_z_`, ~line 225), add:
```cpp
    f32 playable_x0_ = 0, playable_z0_ = 0;
    f32 playable_x1_ = 0, playable_z1_ = 0;
    bool has_playable_rect_ = false;
```

- [ ] **Step 4: Implement SetPlayableRect Lua binding**

In `src/lua/sim_bindings.cpp`, replace line 4123:
```cpp
// Before:
state.register_function("SetPlayableRect", stub_noop);

// After:
state.register_function("SetPlayableRect", [](lua_State* L) -> int {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    f32 x0 = static_cast<f32>(lua_tonumber(L, 1));
    f32 z0 = static_cast<f32>(lua_tonumber(L, 2));
    f32 x1 = static_cast<f32>(lua_tonumber(L, 3));
    f32 z1 = static_cast<f32>(lua_tonumber(L, 4));
    sim->set_playable_rect(x0, z0, x1, z1);
    return 0;
});
```

Also update `GetPlayableRect` (lines 4108-4122) to use stored values when set:
```cpp
state.register_function("GetPlayableRect", [](lua_State* L) -> int {
    auto* sim = get_sim(L);
    lua_newtable(L);
    if (sim) {
        f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        if (sim->has_playable_rect()) {
            x0 = sim->playable_x0(); z0 = sim->playable_z0();
            x1 = sim->playable_x1(); z1 = sim->playable_z1();
        } else if (sim->terrain()) {
            x1 = static_cast<f32>(sim->terrain()->map_width());
            z1 = static_cast<f32>(sim->terrain()->map_height());
        }
        lua_pushnumber(L, 1); lua_pushnumber(L, x0); lua_settable(L, -3);
        lua_pushnumber(L, 2); lua_pushnumber(L, z0); lua_settable(L, -3);
        lua_pushnumber(L, 3); lua_pushnumber(L, x1); lua_settable(L, -3);
        lua_pushnumber(L, 4); lua_pushnumber(L, z1); lua_settable(L, -3);
    }
    return 1;
});
```

Also remove the duplicate `SetPlayableRect` at line 4346 if it exists.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m154]" -v`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/sim/sim_state.hpp src/lua/sim_bindings.cpp
git commit -m "M154b: Implement SetPlayableRect — stores bounds on SimState, updates GetPlayableRect"
```

---

### Task 8: Clamp Navigator Movement to Playable Rect

**Files:**
- Modify: `src/sim/navigator.hpp`
- Modify: `src/sim/navigator.cpp:53-134`

- [ ] **Step 1: Store SimState pointer on Navigator**

Rather than modifying `update()`'s signature (which would require changing 13 call sites in `src/sim/unit.cpp`), store a `const SimState*` on the Navigator and set it during unit initialization.

In `src/sim/navigator.hpp`, add:
```cpp
class SimState; // forward declaration at top of file

// In Navigator class:
private:
    const SimState* sim_ = nullptr;
public:
    void set_sim_state(const SimState* sim) { sim_ = sim; }
```

- [ ] **Step 2: Clamp position in Navigator::update**

In `src/sim/navigator.cpp`, add clamping before each `entity.set_position(pos)` call.

There are 4 calls to `entity.set_position(pos)` in the function (lines 80, 100, 121, 132). Add the clamp before each one:
```cpp
if (sim_) pos = sim_->clamp_to_playable(pos);
entity.set_position(pos);
```

Add `#include "sim/sim_state.hpp"` at the top of navigator.cpp.

- [ ] **Step 3: Set SimState on Navigator during unit creation**

In `src/lua/sim_bindings.cpp`, in `create_unit_core()` (around line 526 where the unit is registered), add:
```cpp
unit->navigator().set_sim_state(sim);
```

This is a single call site change — much cleaner than modifying 13 `navigator_.update()` call sites.

- [ ] **Step 4: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build, no errors

- [ ] **Step 5: Run all tests**

Run: `./build/tests/Debug/osc_tests.exe -v`
Expected: All tests pass

- [ ] **Step 6: Commit**

```bash
git add src/sim/navigator.hpp src/sim/navigator.cpp src/lua/sim_bindings.cpp
git commit -m "M154c: Clamp Navigator movement to playable rect bounds"
```

---

### Task 9: Implement AddBuildRestriction / RemoveBuildRestriction

**Files:**
- Modify: `src/sim/army_brain.hpp:145-172`
- Modify: `src/lua/sim_bindings.cpp:4228-4229`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_smoke_test.cpp`:
```cpp
#include "sim/army_brain.hpp"

TEST_CASE("ArmyBrain build restriction add/remove/check", "[m154]") {
    osc::sim::ArmyBrain brain;

    REQUIRE_FALSE(brain.is_build_restricted("TECH1 LAND FACTORY"));
    brain.add_build_restriction("TECH1 LAND FACTORY");
    REQUIRE(brain.is_build_restricted("TECH1 LAND FACTORY"));
    brain.remove_build_restriction("TECH1 LAND FACTORY");
    REQUIRE_FALSE(brain.is_build_restricted("TECH1 LAND FACTORY"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m154]" -v`
Expected: FAIL — `add_build_restriction` not declared

- [ ] **Step 3: Add build restriction API to ArmyBrain**

In `src/sim/army_brain.hpp`, add to the public section (after `set_skin_name`, ~line 135):
```cpp
    // --- Build restrictions (per-army) ---
    void add_build_restriction(const std::string& category) {
        build_restrictions_.insert(category);
    }
    void remove_build_restriction(const std::string& category) {
        build_restrictions_.erase(category);
    }
    bool is_build_restricted(const std::string& category) const {
        return build_restrictions_.count(category) > 0;
    }
```

In the private section (after `skin_name_`), add:
```cpp
    std::unordered_set<std::string> build_restrictions_;
```

Add `#include <unordered_set>` at the top if not already present.

- [ ] **Step 4: Wire Lua bindings**

In `src/lua/sim_bindings.cpp`, replace lines 4228-4229:
```cpp
// Before:
state.register_function("AddBuildRestriction", stub_noop);
state.register_function("RemoveBuildRestriction", stub_noop);

// After:
state.register_function("AddBuildRestriction", [](lua_State* L) -> int {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    i32 army = resolve_army(L, 1, sim); // 1-based Lua → 0-based C++
    if (army < 0) return 0;
    const char* cat = lua_tostring(L, 2);
    if (!cat) return 0;
    auto* brain = sim->get_army(army);
    if (brain) brain->add_build_restriction(cat);
    return 0;
});
state.register_function("RemoveBuildRestriction", [](lua_State* L) -> int {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    i32 army = resolve_army(L, 1, sim); // 1-based Lua → 0-based C++
    if (army < 0) return 0;
    const char* cat = lua_tostring(L, 2);
    if (!cat) return 0;
    auto* brain = sim->get_army(army);
    if (brain) brain->remove_build_restriction(cat);
    return 0;
});
```

**Army index note:** Uses `resolve_army()` (sim_bindings.cpp:~44) which converts 1-based Lua indices to 0-based C++ and handles string army names.

**Category type note:** FA's global `AddBuildRestriction` may pass an EntityCategory expression rather than a plain string. This simplified implementation stores raw category strings. If the smoke test reveals FA passes EntityCategory objects, the implementer should integrate with the existing EntityCategory bitmask infrastructure instead. Add a code comment noting this.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m154]" -v`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/sim/army_brain.hpp src/lua/sim_bindings.cpp tests/test_smoke_test.cpp
git commit -m "M154d: Implement per-army AddBuildRestriction/RemoveBuildRestriction"
```

---

### Task 10: Implement ArmyInitializePrebuiltUnits

**Files:**
- Modify: `src/lua/sim_bindings.cpp:4220`

- [ ] **Step 1: Implement ArmyInitializePrebuiltUnits**

In `src/lua/sim_bindings.cpp`, replace line 4220:
```cpp
// Before:
state.register_function("ArmyInitializePrebuiltUnits", stub_noop);

// After:
state.register_function("ArmyInitializePrebuiltUnits", [](lua_State* L) -> int {
    // FA calls this with the army name. In skirmish, it's typically a no-op
    // because there are no prebuilt units (those are for campaign saves).
    // Log and return silently — campaign save support is out of scope.
    spdlog::debug("ArmyInitializePrebuiltUnits called for army: {}",
                  lua_tostring(L, 1) ? lua_tostring(L, 1) : "(nil)");
    return 0;
});
```

**Rationale:** In skirmish mode, there are no prebuilt units — the ACU is spawned by `SetupSession`→army brain Lua code, not by prebuilt lists. Full campaign save file parsing is out of scope for the skirmish roadmap. This converts the no-op stub to a documented no-op with logging, so the smoke test can see it was called.

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 3: Run all tests**

Run: `./build/tests/Debug/osc_tests.exe -v`
Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add src/lua/sim_bindings.cpp
git commit -m "M154e: Document ArmyInitializePrebuiltUnits as intentional no-op for skirmish"
```

---

### Task 11: Run Smoke Test and Triage Results

This task is discovery-driven and cannot be fully planned in advance. The implementer should:

- [ ] **Step 1: Run the smoke test**

Run: `./build/Debug/opensupcom.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua" --smoke-test`

This requires FA game files mounted via VFS. The engine must be able to find the `.scd` / `.nx2` files.

- [ ] **Step 2: Analyze the output**

Review the smoke test report. Categorize each entry:
- **MISSING_GLOBAL**: Register as no-op stub if cosmetic, implement if gameplay-affecting
- **MISSING_METHOD**: Add to appropriate metatable with sensible default return
- **PCALL_ERROR**: Diagnose root cause — often a missing field or wrong return type upstream
- Prioritize cascading errors (one missing function causing 50+ downstream errors)

- [ ] **Step 3: Implement fixes for each category**

For each fix:
1. Add the function/method in the appropriate file
2. Build and verify
3. Re-run smoke test to measure progress

- [ ] **Step 4: Re-run smoke test and verify 80%+ error reduction**

Run: `./build/Debug/opensupcom.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua" --smoke-test`
Target: reduce error count by 80%+ from the initial run.

- [ ] **Step 5: Commit all triage fixes**

```bash
git add -u  # Stage only tracked modified files (avoid accidental untracked file inclusion)
git commit -m "M154f: Smoke test triage — fix N missing globals, M missing methods, K errors"
```

---

## Milestone Exit Criteria

### M153 Exit Criteria
- `--smoke-test` CLI flag exists and runs the full boot sequence
- SmokeTestHarness intercepts and categorizes: MISSING_GLOBAL, MISSING_METHOD, PCALL_ERROR
- Report is deduplicated (by category+name) with occurrence counts
- Smoke test runs to completion without hard crash
- 8+ unit tests pass for SmokeTestHarness infrastructure

### M154 Exit Criteria
- `IsGameOver()` returns true when `player_result() != 0`
- `SetPlayableRect(x0, z0, x1, z1)` stores bounds on SimState
- `GetPlayableRect()` returns stored bounds (or map size if not set)
- Navigator clamps unit positions to playable rect
- `AddBuildRestriction`/`RemoveBuildRestriction` manage per-army restriction sets on ArmyBrain
- `ArmyInitializePrebuiltUnits` is a documented no-op for skirmish
- Smoke test error count reduced by 80%+ from initial run
- All existing tests still pass (no regressions)
