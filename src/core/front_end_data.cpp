#include "core/front_end_data.hpp"

namespace osc {

void FrontEndData::set(lua_State* L, const std::string& key, int value_idx) {
    auto it = refs_.find(key);
    if (it != refs_.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
    }
    lua_pushvalue(L, value_idx);
    refs_[key] = luaL_ref(L, LUA_REGISTRYINDEX);
}

void FrontEndData::get(lua_State* L, const std::string& key) {
    auto it = refs_.find(key);
    if (it != refs_.end()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
    } else {
        lua_pushnil(L);
    }
}

void FrontEndData::clear(lua_State* L) {
    for (auto& [k, ref] : refs_) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    refs_.clear();
}

} // namespace osc
