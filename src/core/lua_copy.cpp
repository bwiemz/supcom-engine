#include "core/lua_copy.hpp"

#include <unordered_map>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::core {

namespace {

/// Deeper nesting than any sync table carries; stops runaway structures.
constexpr int kMaxCopyDepth = 64;

bool is_plain_key(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNUMBER || t == LUA_TSTRING || t == LUA_TBOOLEAN;
}

bool is_plain_value(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNIL || t == LUA_TNUMBER || t == LUA_TSTRING ||
           t == LUA_TBOOLEAN || t == LUA_TTABLE;
}

class Copier {
public:
    Copier(lua_State* from, lua_State* to) : from_(from), to_(to) {}
    ~Copier() {
        for (const auto& entry : copies_) luaL_unref(to_, LUA_REGISTRYINDEX, entry.second);
    }
    Copier(const Copier&) = delete;
    Copier& operator=(const Copier&) = delete;

    /// Push the copy of from_[idx] (an absolute index) onto to_.
    void copy(int idx, int depth) {
        switch (lua_type(from_, idx)) {
        case LUA_TNUMBER: lua_pushnumber(to_, lua_tonumber(from_, idx)); return;
        case LUA_TBOOLEAN: lua_pushboolean(to_, lua_toboolean(from_, idx)); return;
        case LUA_TSTRING:
            lua_pushlstring(to_, lua_tostring(from_, idx), lua_strlen(from_, idx));
            return;
        case LUA_TTABLE: copy_table(idx, depth); return;
        default: lua_pushnil(to_); return;
        }
    }

private:
    void copy_table(int idx, int depth) {
        const void* key = lua_topointer(from_, idx);
        if (auto it = copies_.find(key); it != copies_.end()) {
            lua_rawgeti(to_, LUA_REGISTRYINDEX, it->second);
            return;
        }
        lua_newtable(to_);
        lua_pushvalue(to_, -1);
        copies_.emplace(key, luaL_ref(to_, LUA_REGISTRYINDEX));
        // Each level holds a key/value pair on `from` and a table plus a
        // key on `to`; past the depth cap, or without stack, stay empty.
        if (depth >= kMaxCopyDepth || !lua_checkstack(from_, 4) ||
            !lua_checkstack(to_, 4)) {
            return;
        }

        const int dst = lua_gettop(to_);
        lua_pushnil(from_);
        while (lua_next(from_, idx) != 0) {
            const int value = lua_gettop(from_);
            const int k = value - 1;
            if (is_plain_key(from_, k) && is_plain_value(from_, value)) {
                copy(k, depth + 1);
                copy(value, depth + 1);
                lua_rawset(to_, dst);
            }
            lua_pop(from_, 1); // value; key stays for lua_next
        }
    }

    lua_State* from_;
    lua_State* to_;
    std::unordered_map<const void*, int> copies_; // source table -> registry ref
};

} // namespace

void copy_lua_value(lua_State* from, int from_idx, lua_State* to) {
    if (from_idx < 0) from_idx = lua_gettop(from) + from_idx + 1;
    Copier(from, to).copy(from_idx, 0);
}

} // namespace osc::core
