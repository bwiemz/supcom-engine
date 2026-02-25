#include "lua/category_utils.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

static bool match_impl(lua_State* L, int cat_idx,
                        const std::unordered_set<std::string>& cats,
                        int depth) {
    if (depth > 16) return false; // guard against pathological nesting

    // Normalise to absolute index before any stack manipulation
    if (cat_idx < 0) cat_idx = lua_gettop(L) + cat_idx + 1;

    if (!lua_istable(L, cat_idx)) return false;

    // 1. Simple category: { __name = "COMMAND" }
    lua_pushstring(L, "__name");
    lua_rawget(L, cat_idx);
    if (lua_isstring(L, -1)) {
        std::string name = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (name == "ALLUNITS") return true;
        return cats.count(name) > 0;
    }
    lua_pop(L, 1);

    // 2. Compound category: { __op, __left, __right }
    lua_pushstring(L, "__op");
    lua_rawget(L, cat_idx);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    std::string op = lua_tostring(L, -1);
    lua_pop(L, 1);

    // Get left operand
    lua_pushstring(L, "__left");
    lua_rawget(L, cat_idx);
    int left_idx = lua_gettop(L);

    // Get right operand
    lua_pushstring(L, "__right");
    lua_rawget(L, cat_idx);
    int right_idx = lua_gettop(L);

    bool result = false;
    if (op == "union") {
        result = match_impl(L, left_idx, cats, depth + 1) ||
                 match_impl(L, right_idx, cats, depth + 1);
    } else if (op == "intersection") {
        result = match_impl(L, left_idx, cats, depth + 1) &&
                 match_impl(L, right_idx, cats, depth + 1);
    } else if (op == "difference") {
        result = match_impl(L, left_idx, cats, depth + 1) &&
                 !match_impl(L, right_idx, cats, depth + 1);
    }

    lua_pop(L, 2); // pop left + right
    return result;
}

bool unit_matches_category(lua_State* L, int cat_idx,
                           const std::unordered_set<std::string>& unit_cats) {
    return match_impl(L, cat_idx, unit_cats, 0);
}

bool categories_match(lua_State* L, int cat_idx,
                      const std::unordered_set<std::string>& cats) {
    return match_impl(L, cat_idx, cats, 0);
}

} // namespace osc::lua
