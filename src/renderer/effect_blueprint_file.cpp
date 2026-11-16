#include "renderer/effect_blueprint_file.hpp"

#include <lua.h>

#include <cctype>

namespace osc::renderer {

bool read_effect_blueprint(const blueprints::BlueprintStore* store, lua_State* L,
                           const std::string& name, blueprints::BlueprintType type,
                           const std::function<void(lua_State*, int)>& parse) {
    // Its table is in the store's Lua state: in another, its reference would
    // name some other value.
    const blueprints::BlueprintEntry* entry =
        store && L && store->lua_state() == L ? store->find_effect(name, type) : nullptr;
    if (!entry) return false;
    const int top = lua_gettop(L);
    store->push_lua_table(*entry, L);
    // (None while the store waits for the next game's: rebind drops them)
    const bool ok = lua_istable(L, -1);
    if (ok) parse(L, lua_gettop(L));
    lua_settop(L, top);
    return ok;
}

f32 blueprint_number(lua_State* L, int idx, const char* key, f32 fallback) {
    lua_pushstring(L, key);
    lua_rawget(L, idx);
    const f32 v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

u8 blueprint_fidelity(lua_State* L, int idx) {
    u8 mask = 0;
    const char* const keys[] = {"LowFidelity", "MedFidelity", "HighFidelity"};
    for (u32 i = 0; i < 3; ++i) {
        lua_pushstring(L, keys[i]);
        lua_rawget(L, idx);
        const bool off = lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) == 0;
        lua_pop(L, 1);
        if (!off) mask = static_cast<u8>(mask | (1u << i));
    }
    return mask;
}

std::string blueprint_path(lua_State* L, int idx, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, idx);
    std::string out;
    if (lua_type(L, -1) == LUA_TSTRING) {
        out = lua_tostring(L, -1);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    lua_pop(L, 1);
    return out;
}

} // namespace osc::renderer
