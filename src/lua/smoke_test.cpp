#include "lua/smoke_test.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

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

// C callback for method __index interceptor — lives outside the namespace
// because it is a plain C function registered with lua_pushcclosure.
// Upvalue layout:
//   upvalue 1 = original __index (the metatable itself)
//   upvalue 2 = type_name string
//   upvalue 3 = harness lightuserdata
static int smoke_method_index(lua_State* L) {
    // Stack: object (1), key (2)
    // First try the original metatable via rawget
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

// C callback for globals __index metamethod — lives outside the namespace
// because it is a plain C function registered with lua_pushcfunction.
static int smoke_global_index(lua_State* L) {
    // Stack on entry: globals_table (index 1), key (index 2)
    // __index is only called when the key is NOT found via raw lookup in the
    // globals table, so we know the access is missing — log it and return nil.
    const char* key = lua_tostring(L, 2);
    if (!key) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, "__osc_smoke_harness");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* harness = static_cast<osc::lua::SmokeTestHarness*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (harness) {
        lua_Debug ar;
        std::string location = "?";
        if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
            location = std::string(ar.source ? ar.source : "?") +
                       ":" + std::to_string(ar.currentline);
        }
        harness->record(osc::lua::SmokeCategory::MissingGlobal, key, location);
    }

    lua_pushnil(L);
    return 1;
}

namespace osc::lua {

void SmokeTestHarness::install_global_interceptor(lua_State* L) {
    // Store the harness pointer in the registry so the C callback can reach it.
    lua_pushstring(L, "__osc_smoke_harness");
    lua_pushlightuserdata(L, this);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Build metatable: { __index = smoke_global_index }
    lua_newtable(L);
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, smoke_global_index);
    lua_rawset(L, -3);

    // Attach the metatable to the globals table.
    // lua_setmetatable pops the table from the stack.
    lua_setmetatable(L, LUA_GLOBALSINDEX);
}

void SmokeTestHarness::install_method_interceptor(
    lua_State* L, const char* registry_key, const char* type_name) {
    // Get the cached metatable from the registry
    lua_pushstring(L, registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return; // Metatable not yet created — skip
    }
    int mt = lua_gettop(L);

    // Replace __index with our interceptor closure.
    // The closure captures: (1) original metatable, (2) type_name, (3) harness ptr
    lua_pushstring(L, "__index");
    lua_pushvalue(L, mt);                          // upvalue 1: original mt
    lua_pushstring(L, type_name);                  // upvalue 2: type name
    lua_pushlightuserdata(L, this);                // upvalue 3: harness
    lua_pushcclosure(L, smoke_method_index, 3);
    lua_rawset(L, mt);  // mt.__index = closure

    lua_pop(L, 1); // pop mt
}

void SmokeTestHarness::install_all_method_interceptors(lua_State* L) {
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

    // Also intercept unit_methods and entity_methods via the moho table.
    // These use a different pattern: moho.unit_methods is the __index target.
    // We install a meta-metatable on those tables with an __index closure.
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        struct MohoEntry { const char* field; const char* name; };
        static const MohoEntry moho_entries[] = {
            {"unit_methods",   "Unit"},
            {"entity_methods", "Entity"},
            {"army_methods",   "ArmyBrain"},
        };
        int moho_idx = lua_gettop(L);
        for (auto& me : moho_entries) {
            lua_pushstring(L, me.field);
            lua_rawget(L, moho_idx);
            if (lua_istable(L, -1)) {
                // This table IS the __index target. We set a metatable on IT
                // with an __index handler for missing keys.
                lua_newtable(L); // meta-metatable for the methods table
                lua_pushstring(L, "__index");
                lua_newtable(L);                      // upvalue 1 (unused dummy)
                lua_pushstring(L, me.name);           // upvalue 2
                lua_pushlightuserdata(L, this);        // upvalue 3
                lua_pushcclosure(L, smoke_method_index, 3);
                lua_rawset(L, -3); // meta_mt.__index = closure
                lua_setmetatable(L, -2); // set meta-metatable on moho.X_methods
            }
            lua_pop(L, 1); // pop the field value
        }
    }
    lua_pop(L, 1); // pop moho
}

} // namespace osc::lua
