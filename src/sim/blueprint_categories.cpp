#include "sim/blueprint_categories.hpp"

extern "C" {
#include <lua.h>
}

namespace osc::sim {

void collect_blueprint_categories(lua_State* L, int bp_index,
                                  std::unordered_set<std::string>& out) {
    if (bp_index < 0 && bp_index > LUA_REGISTRYINDEX) {
        bp_index = lua_gettop(L) + bp_index + 1;
    }
    if (!lua_istable(L, bp_index)) return;

    // FAF: CategoriesHash = { COMMAND = true, ... }
    lua_pushstring(L, "CategoriesHash");
    lua_gettable(L, bp_index);
    if (lua_istable(L, -1)) {
        const int hash = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, hash) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) out.insert(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // Retail: Categories = { 'COMMAND', ... }
    lua_pushstring(L, "Categories");
    lua_gettable(L, bp_index);
    if (lua_istable(L, -1)) {
        const int list = lua_gettop(L);
        for (int i = 1;; ++i) {
            lua_rawgeti(L, list, i);
            const int type = lua_type(L, -1);
            if (type == LUA_TNIL) {
                lua_pop(L, 1);
                break;
            }
            if (type == LUA_TSTRING) out.insert(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

} // namespace osc::sim
