#pragma once

#include <string>
#include <unordered_map>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc {

/// Simple key->Lua-value store for cross-state communication.
/// Used by GetFrontEndData / SetFrontEndData to pass data between
/// front-end (lobby) and loading/game states.
/// Values are stored as Lua registry refs.
class FrontEndData {
public:
    void set(lua_State* L, const std::string& key, int value_idx);
    void get(lua_State* L, const std::string& key);
    void clear(lua_State* L);

private:
    std::unordered_map<std::string, int> refs_; // key -> luaL_ref
};

} // namespace osc
