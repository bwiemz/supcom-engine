#pragma once

/// Common Lua C stub functions used across moho_bindings and sim_bindings.
/// All are `inline` so they can live in a header included by multiple TUs.
/// Kept in a short namespace to avoid collisions with existing osc:: types.

extern "C" {
#include <lua.h>
}

namespace lua_stubs {

inline int noop(lua_State*) { return 0; }
inline int return_nil(lua_State* L) { lua_pushnil(L); return 1; }
inline int return_false(lua_State* L) { lua_pushboolean(L, 0); return 1; }
inline int return_true(lua_State* L) { lua_pushboolean(L, 1); return 1; }
inline int return_zero(lua_State* L) { lua_pushnumber(L, 0); return 1; }
inline int return_one(lua_State* L) { lua_pushnumber(L, 1); return 1; }
inline int return_empty_table(lua_State* L) { lua_newtable(L); return 1; }
inline int return_self(lua_State* L) { lua_pushvalue(L, 1); return 1; }
inline int return_empty_string(lua_State* L) { lua_pushstring(L, ""); return 1; }
inline int return_1000(lua_State* L) { lua_pushnumber(L, 1000); return 1; }

} // namespace lua_stubs
