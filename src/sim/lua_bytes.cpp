#include "sim/lua_bytes.hpp"

#include "core/types.hpp"
#include "sim/command_codec.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {

enum Tag : u8 {
    kNil = 0,
    kFalse = 1,
    kTrue = 2,
    kNumber = 3,
    kString = 4,
    kTable = 5,
};

bool is_key_type(int type) {
    return type == LUA_TBOOLEAN || type == LUA_TNUMBER || type == LUA_TSTRING;
}

bool is_data_type(int type) {
    return is_key_type(type) || type == LUA_TTABLE;
}

/// A table key, ordered numbers first (ascending), then strings, then
/// false and true.
struct Key {
    int rank = 0; // 0 number, 1 string, 2 boolean
    f64 number = 0.0;
    std::string string;
    bool boolean = false;

    bool operator<(const Key& o) const {
        return std::tie(rank, number, string, boolean) <
               std::tie(o.rank, o.number, o.string, o.boolean);
    }
};

class Encoder {
public:
    Encoder(lua_State* L, std::vector<u8>& out) : L_(L), out_(out), w_(out) {}

    /// Write the value at absolute index `idx`.
    bool value(int idx, int depth) {
        if (out_.size() > kLuaBytesMaxSize) return false;
        switch (lua_type(L_, idx)) {
        case LUA_TBOOLEAN: w_.u8v(lua_toboolean(L_, idx) ? kTrue : kFalse); return true;
        case LUA_TNUMBER:
            w_.u8v(kNumber);
            w_.f64v(lua_tonumber(L_, idx));
            return true;
        case LUA_TSTRING:
            w_.u8v(kString);
            w_.str(std::string(lua_tostring(L_, idx), lua_strlen(L_, idx)));
            return true;
        case LUA_TTABLE: return table(idx, depth);
        default: w_.u8v(kNil); return true; // (a table's entries are filtered first)
        }
    }

private:
    bool table(int idx, int depth) {
        // (A table inside itself fails here on the first path around it: a
        // failure ends the whole write.)
        if (depth >= kLuaBytesMaxDepth || !lua_checkstack(L_, 4)) return false;

        std::vector<Key> keys;
        lua_pushnil(L_);
        while (lua_next(L_, idx) != 0) {
            if (is_data_type(lua_type(L_, -1)) && is_key_type(lua_type(L_, -2)))
                keys.push_back(key_at(-2));
            lua_pop(L_, 1);
        }
        std::sort(keys.begin(), keys.end());

        w_.u8v(kTable);
        w_.u32v(static_cast<u32>(keys.size()));
        for (const Key& key : keys) {
            push_key(key);
            const int at = lua_gettop(L_);
            (void)value(at, depth + 1); // a key: never a table
            lua_pushvalue(L_, at);
            lua_rawget(L_, idx);
            const bool ok = value(at + 1, depth + 1);
            lua_pop(L_, 2);
            if (!ok) return false;
        }
        return true;
    }

    /// The key at `idx`, read without converting it in place (lua_next
    /// needs it as it is).
    Key key_at(int idx) const {
        Key key;
        switch (lua_type(L_, idx)) {
        case LUA_TNUMBER: key.number = lua_tonumber(L_, idx); break;
        case LUA_TSTRING:
            key.rank = 1;
            key.string.assign(lua_tostring(L_, idx), lua_strlen(L_, idx));
            break;
        default:
            key.rank = 2;
            key.boolean = lua_toboolean(L_, idx) != 0;
            break;
        }
        return key;
    }

    void push_key(const Key& key) const {
        if (key.rank == 0) lua_pushnumber(L_, key.number);
        else if (key.rank == 1) lua_pushlstring(L_, key.string.data(), key.string.size());
        else lua_pushboolean(L_, key.boolean ? 1 : 0);
    }

    lua_State* L_;
    const std::vector<u8>& out_;
    ByteWriter w_;
};

class Decoder {
public:
    Decoder(lua_State* L, const std::vector<u8>& in) : L_(L), r_(in) {}

    /// Push the next value. On failure the stack may hold partial values:
    /// the caller restores it.
    bool value(int depth) {
        if (!lua_checkstack(L_, 3)) return false;
        const u8 tag = r_.u8v();
        if (!r_.ok()) return false;
        switch (tag) {
        case kNil: lua_pushnil(L_); return true;
        case kFalse: lua_pushboolean(L_, 0); return true;
        case kTrue: lua_pushboolean(L_, 1); return true;
        case kNumber: {
            const f64 v = r_.f64v();
            if (!r_.ok()) return false;
            lua_pushnumber(L_, v);
            return true;
        }
        case kString: {
            const std::string s = r_.str();
            if (!r_.ok()) return false;
            lua_pushlstring(L_, s.data(), s.size());
            return true;
        }
        case kTable: {
            if (depth >= kLuaBytesMaxDepth) return false;
            // The count comes from the bytes: read until it is met or they
            // run out, never reserving for it.
            const u32 n = r_.u32v();
            if (!r_.ok()) return false;
            lua_newtable(L_);
            for (u32 i = 0; i < n; ++i) {
                if (!value(depth + 1)) return false;
                const int key_type = lua_type(L_, -1);
                // (lua_rawset raises on a NaN key)
                if (!is_key_type(key_type) ||
                    (key_type == LUA_TNUMBER && std::isnan(lua_tonumber(L_, -1))))
                    return false;
                if (!value(depth + 1) || lua_isnil(L_, -1)) return false;
                lua_rawset(L_, -3);
            }
            return true;
        }
        default: return false;
        }
    }

    const ByteReader& reader() const { return r_; }

private:
    lua_State* L_;
    ByteReader r_;
};

} // namespace

std::optional<std::string> lua_to_bytes(lua_State* L, int idx) {
    const int top = lua_gettop(L);
    if (idx < 0 && idx > LUA_REGISTRYINDEX) idx = top + idx + 1;
    std::vector<u8> out;
    Encoder encoder(L, out);
    const bool ok = encoder.value(idx, 0) && out.size() <= kLuaBytesMaxSize;
    lua_settop(L, top);
    if (!ok) return std::nullopt;
    return std::string(out.begin(), out.end());
}

bool push_lua_bytes(lua_State* L, std::string_view bytes) {
    const std::vector<u8> in(bytes.begin(), bytes.end());
    const int top = lua_gettop(L);
    Decoder decoder(L, in);
    if (decoder.value(0) && decoder.reader().ok() && decoder.reader().position() == in.size())
        return true;
    lua_settop(L, top);
    return false;
}

} // namespace osc::sim
