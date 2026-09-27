#include "sim/game_colors.hpp"

#include "core/color.hpp"

#include <spdlog/spdlog.h>

#include <string_view>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {

/// The colours of the list at the top of the stack, decoded; white for one
/// Moho couldn't decode either.
std::vector<u32> decode_list(lua_State* L) {
    std::vector<u32> colors;
    if (!lua_istable(L, -1)) return colors;
    for (int i = 1;; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            break;
        }
        const auto color = decode_color(std::string_view(lua_tostring(L, -1), lua_strlen(L, -1)));
        colors.push_back(color.value_or(0xFFFFFFFFu));
        lua_pop(L, 1);
    }
    return colors;
}

} // namespace

GameColors read_game_colors(lua_State* L) {
    GameColors out;
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "/lua/GameColors.lua");
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
            lua_pushstring(L, "GameColors");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "ArmyColors");
                lua_gettable(L, -2);
                out.army_colors = decode_list(L);
                lua_pop(L, 1);
                lua_pushstring(L, "PlayerColors");
                lua_gettable(L, -2);
                out.player_colors = decode_list(L);
                lua_pop(L, 1);
            }
        } else {
            spdlog::warn("GameColors: {}",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "not loaded");
        }
    }
    lua_settop(L, top);
    return out;
}

} // namespace osc::sim
