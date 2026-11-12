#include "lua/user_options.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::lua {

namespace {

/// import('/lua/user/prefs.lua').GetOption(key), run protected: Moho's
/// SCR_Import and LuaPlus call raise, and OPTIONS_Get* catch it. Any step
/// failing (import missing, the module not a table, GetOption not a
/// function, the script's own error) is an error here.
int call_get_option(lua_State* L) {
    lua_pushstring(L, "import");
    lua_gettable(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "/lua/user/prefs.lua");
    lua_call(L, 1, 1);
    lua_pushstring(L, "GetOption");
    lua_gettable(L, -2);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    return 1;
}

/// Pushes GetOption(key)'s answer and answers true; or warns as Moho does,
/// pushes nothing and answers false.
bool push_option(lua_State* L, const char* key) {
    lua_pushcfunction(L, call_get_option);
    lua_pushstring(L, key);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        spdlog::warn("Error running '/lua/user/prefs.lua:GetOption' : {}", err ? err : "");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

} // namespace

int options_get_int(lua_State* L, const char* key) {
    if (!push_option(L, key)) return 0;
    const int value = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return value;
}

f32 options_get_float(lua_State* L, const char* key) {
    if (!push_option(L, key)) return 0.0f;
    const auto value = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return value;
}

std::string options_get_string(lua_State* L, const char* key) {
    if (!push_option(L, key)) return {};
    const char* text = lua_tostring(L, -1);
    std::string value = text ? text : "";
    lua_pop(L, 1);
    return value;
}

bool options_get_bool(lua_State* L, const char* key) {
    if (!push_option(L, key)) return false;
    const bool value = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}

} // namespace osc::lua
