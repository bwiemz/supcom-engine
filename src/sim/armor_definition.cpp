#include "sim/armor_definition.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstdlib>
#include <cstring>

namespace osc::sim {

void ArmorDefinition::load_from_lua(lua_State* L) {
    // Expects the "armordefinition" table on top of stack.
    // It's an array of arrays. Each sub-array:
    //   [1] = armor type name (string)
    //   [2..n] = "DamageType multiplier" (strings)
    if (!lua_istable(L, -1)) return;

    int tbl = lua_gettop(L);
    for (int i = 1; ; i++) {
        lua_rawgeti(L, tbl, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        int entry = lua_gettop(L);

        // [1] = armor type name
        lua_rawgeti(L, entry, 1);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2); // name + entry
            continue;
        }
        std::string armor_type = lua_tostring(L, -1);
        lua_pop(L, 1);

        auto& type_map = table_[armor_type];

        // [2..n] = "DamageType multiplier" strings
        for (int j = 2; ; j++) {
            lua_rawgeti(L, entry, j);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); continue; }

            const char* str = lua_tostring(L, -1);
            // Format: "DamageType multiplier" — split on last space
            const char* space = std::strrchr(str, ' ');
            if (space && space > str) {
                std::string damage_type(str, space);
                f32 mult = static_cast<f32>(std::atof(space + 1));
                type_map[damage_type] = mult;
            }
            lua_pop(L, 1);
        }

        spdlog::debug("  ArmorType '{}': {} damage modifiers",
                       armor_type, type_map.size());
        lua_pop(L, 1); // pop entry table
    }
}

f32 ArmorDefinition::get_multiplier(const std::string& armor_type,
                                     const std::string& damage_type) const {
    auto it = table_.find(armor_type);
    if (it == table_.end()) return 1.0f;
    auto jt = it->second.find(damage_type);
    if (jt == it->second.end()) return 1.0f;
    return jt->second;
}

void ArmorDefinition::set_multiplier(const std::string& armor_type,
                                      const std::string& damage_type,
                                      f32 mult) {
    table_[armor_type][damage_type] = mult;
}

} // namespace osc::sim
