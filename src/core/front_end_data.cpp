#include "core/front_end_data.hpp"
#include "core/lua_copy.hpp"

extern "C" {
#include <lauxlib.h>
}

namespace osc {

FrontEndData::FrontEndData() : store_(lua_open()) {}

FrontEndData::~FrontEndData() {
    lua_close(store_);
}

void FrontEndData::set(lua_State* L, const std::string& key, int value_idx) {
    if (value_idx < 0) value_idx = lua_gettop(L) + value_idx + 1;
    lua_pushlstring(store_, key.data(), key.size());
    core::copy_lua_value(L, value_idx, store_);
    lua_rawset(store_, LUA_GLOBALSINDEX);
}

void FrontEndData::get(lua_State* L, const std::string& key) const {
    lua_pushlstring(store_, key.data(), key.size());
    lua_rawget(store_, LUA_GLOBALSINDEX);
    core::copy_lua_value(store_, -1, L);
    lua_pop(store_, 1);
}

void FrontEndData::clear() {
    lua_close(store_);
    store_ = lua_open();
}

} // namespace osc
