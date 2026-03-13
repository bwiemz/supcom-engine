#include "lua/beat_system.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

void BeatFunctionRegistry::add(lua_State* L, int func_idx, const std::string& name) {
    lua_pushvalue(L, func_idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    entries_.push_back({ref, name});
    spdlog::debug("BeatFunction added: ref={} name='{}'", ref, name);
}

void BeatFunctionRegistry::remove(lua_State* L, int func_idx) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->lua_ref);
        lua_pushvalue(L, func_idx);
        bool equal = lua_equal(L, -1, -2) != 0;
        lua_pop(L, 2);
        if (equal) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->lua_ref);
            entries_.erase(it);
            return;
        }
    }
}

void BeatFunctionRegistry::remove_by_name(const std::string& name, lua_State* L) {
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->name == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->lua_ref);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void BeatFunctionRegistry::fire_all(lua_State* L) {
    // Snapshot refs to avoid iterator invalidation
    std::vector<std::pair<int, std::string>> snapshot;
    snapshot.reserve(entries_.size());
    for (const auto& e : entries_) {
        snapshot.emplace_back(e.lua_ref, e.name);
    }
    for (const auto& [ref, name] : snapshot) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != 0) {
                spdlog::warn("BeatFunction '{}' error: {}", name, lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }
}

void BeatFunctionRegistry::clear(lua_State* L) {
    for (auto& e : entries_) {
        luaL_unref(L, LUA_REGISTRYINDEX, e.lua_ref);
    }
    entries_.clear();
}

} // namespace osc::lua
