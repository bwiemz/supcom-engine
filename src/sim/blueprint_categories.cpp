#include "sim/blueprint_categories.hpp"

extern "C" {
#include <lua.h>
}

#include <sstream>

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

namespace {

/// An Economy.BuildableCategory entry: the blueprint's id, or category
/// tokens all of which the target has.
bool entry_allows(const std::string& entry, const std::string& target_bp,
                  const std::unordered_set<std::string>& target_cats) {
    if (entry == target_bp) return true; // upgrade paths name a blueprint
    std::istringstream ss(entry);
    std::string token;
    bool any = false;
    while (ss >> token) {
        if (target_cats.count(token) == 0) return false;
        any = true;
    }
    return any;
}

} // namespace

bool blueprint_can_build(lua_State* L, const std::string& builder_bp,
                         const std::string& target_bp) {
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    const int bps = lua_gettop(L);

    std::unordered_set<std::string> target_cats;
    lua_pushstring(L, target_bp.c_str());
    lua_rawget(L, bps);
    if (lua_istable(L, -1)) collect_blueprint_categories(L, lua_gettop(L), target_cats);
    lua_pop(L, 1);

    bool can = false;
    lua_pushstring(L, builder_bp.c_str());
    lua_rawget(L, bps);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Economy");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "BuildableCategory");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                const int bc = lua_gettop(L);
                for (int i = 1; !can; ++i) {
                    lua_rawgeti(L, bc, i);
                    if (lua_isnil(L, -1)) break;
                    if (lua_type(L, -1) == LUA_TSTRING)
                        can = entry_allows(lua_tostring(L, -1), target_bp, target_cats);
                    lua_pop(L, 1);
                }
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                can = entry_allows(lua_tostring(L, -1), target_bp, target_cats);
            }
        }
    }
    lua_settop(L, top);
    return can;
}

} // namespace osc::sim
