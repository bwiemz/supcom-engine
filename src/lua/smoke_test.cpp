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

} // namespace osc::lua
