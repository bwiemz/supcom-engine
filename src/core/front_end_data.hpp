#pragma once

#include <string>

extern "C" {
#include <lua.h>
}

namespace osc {

/// What the front end, the lobby and the game hand each other
/// (SetFrontEndData / GetFrontEndData): the session config, a replay's file
/// name, the next operation's briefing. Moho gives the front end and each
/// game a Lua state of their own, and this data outlives them, so values are
/// kept as deep copies in a private Lua state: plain data only (nil,
/// booleans, numbers, strings and tables of them), as core::copy_lua_value
/// copies it. Each get is a fresh copy.
class FrontEndData {
public:
    FrontEndData();
    ~FrontEndData();
    FrontEndData(const FrontEndData&) = delete;
    FrontEndData& operator=(const FrontEndData&) = delete;

    /// Keep a copy of the value at `value_idx` in L under `key`; nil
    /// removes the key.
    void set(lua_State* L, const std::string& key, int value_idx);
    /// Push a copy of `key`'s value onto L, or nil.
    void get(lua_State* L, const std::string& key) const;
    void clear();

private:
    lua_State* store_; ///< its globals table holds the values, by key
};

} // namespace osc
