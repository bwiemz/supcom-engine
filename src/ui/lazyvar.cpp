#include "ui/lazyvar.hpp"

extern "C" {
#include <lua.h>
}

namespace osc::ui {

f32 read_lazyvar(lua_State* L, int table_idx, const char* field) {
    if (table_idx < 0) {
        table_idx = lua_gettop(L) + table_idx + 1;
    }

    lua_pushstring(L, field);
    lua_rawget(L, table_idx);
    if (lua_isnumber(L, -1)) {
        const f32 val = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return val;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0.0f;
    }
    // A LazyVar is called for its value (__call)
    if (lua_pcall(L, 0, 1, 0) != 0) {
        lua_pop(L, 1);
        return 0.0f;
    }
    const f32 val = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return val;
}

} // namespace osc::ui
