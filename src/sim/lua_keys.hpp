#pragma once

// The table keys the engine reads on nearly every binding call: a script
// object's `_c_object`, a category's `__name`. lua_pushstring hashes its
// string and looks it up in the state's string table each time; a host key
// (lua_pushhostkey, an OpenSupCom addition to Lua) is interned once per state
// and kept, so pushing it again hashes nothing. A read gives the same value
// either way: a Lua string is its contents.

#include <cstddef>
#include <iterator>

#include <lhost.h>
#include <lua.h>

namespace osc::sim {

/// A key, by its slot in a state's host keys.
enum class LuaKey : int {
    CObject,   // "_c_object": a script object's C++ object
    CUnit,     // "_c_unit": a weapon's unit
    CSimGen,   // "_c_sim_gen": the sim generation a handle belongs to
    CEntityId, // "_c_entity_id": a UI unit object's entity
    Name,      // "__name": a category's name
    Op,        // "__op": a category expression's operation
    Left,      // "__left": its operands
    Right,     // "__right"
    Count
};

/// Push `key`'s string, as lua_pushstring would.
inline void push_key(lua_State* L, LuaKey key) {
    static constexpr const char* kNames[] = {
        "_c_object", "_c_unit", "_c_sim_gen", "_c_entity_id", "__name", "__op", "__left", "__right",
    };
    static_assert(std::size(kNames) == static_cast<std::size_t>(LuaKey::Count));
    static_assert(static_cast<int>(LuaKey::Count) <= LUA_HOSTKEYS);
    const int k = static_cast<int>(key);
    lua_pushhostkey(L, k, kNames[k]);
}

/// Push t[key], raw, for the table t at `idx`. As `lua_pushstring(L, name);
/// lua_rawget(L, idx);`, so a negative `idx` counts the pushed key too.
inline void rawget_key(lua_State* L, int idx, LuaKey key) {
    push_key(L, key);
    lua_rawget(L, idx);
}

} // namespace osc::sim
