#include "lua/lobby_wire.hpp"

extern "C" {
#include <lua.h>
}

#include <cstring>
#include <string>
#include <vector>

namespace osc::lua {

namespace {

enum Tag : u8 { kNumber = 0, kString = 1, kNil = 2, kBool = 3, kTable = 4, kEnd = 5 };

int absolute(lua_State* L, int idx) {
    return idx > 0 || idx <= LUA_REGISTRYINDEX ? idx : lua_gettop(L) + idx + 1;
}

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void encode(lua_State* L, int idx, int depth, std::vector<const void*>& open,
            std::vector<u8>& out) {
    idx = absolute(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER: {
        const double d = lua_tonumber(L, idx);
        u64 bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        out.push_back(kNumber);
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(bits >> (8 * i)));
        return;
    }
    case LUA_TSTRING: {
        const size_t n = lua_strlen(L, idx);
        const char* s = lua_tostring(L, idx);
        out.push_back(kString);
        put_u32(out, static_cast<u32>(n));
        out.insert(out.end(), s, s + n);
        return;
    }
    case LUA_TBOOLEAN:
        out.push_back(kBool);
        out.push_back(lua_toboolean(L, idx) ? 1 : 0);
        return;
    case LUA_TTABLE: {
        const void* t = lua_topointer(L, idx);
        bool cycle = false;
        for (const void* o : open) cycle = cycle || o == t;
        // Each level holds a key and a value on the stack as it goes
        if (depth >= kLobbyWireMaxDepth || cycle || !lua_checkstack(L, 4)) {
            out.push_back(kNil);
            return;
        }
        open.push_back(t);
        out.push_back(kTable);
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            // A key that can't travel drops its pair
            const int kt = lua_type(L, -2);
            if (kt == LUA_TNUMBER || kt == LUA_TSTRING || kt == LUA_TBOOLEAN) {
                encode(L, -2, depth + 1, open, out);
                encode(L, -1, depth + 1, open, out);
            }
            lua_pop(L, 1);
        }
        out.push_back(kEnd);
        open.pop_back();
        return;
    }
    default:
        out.push_back(kNil); // nil, and what can't travel
        return;
    }
}

class Decoder {
public:
    explicit Decoder(const std::vector<u8>& b) : b_(b) {}
    /// Push one value; false (pushing nothing) if malformed.
    bool value(lua_State* L, int depth) {
        if (at_ >= b_.size() || depth > kLobbyWireMaxDepth) return false;
        switch (b_[at_++]) {
        case kNumber: {
            if (at_ + 8 > b_.size()) return false;
            u64 bits = 0;
            for (int i = 0; i < 8; ++i)
                bits |= static_cast<u64>(b_[at_ + static_cast<size_t>(i)]) << (8 * i);
            at_ += 8;
            double d = 0;
            std::memcpy(&d, &bits, sizeof(d));
            lua_pushnumber(L, d);
            return true;
        }
        case kString: {
            if (at_ + 4 > b_.size()) return false;
            u32 n = 0;
            for (int i = 0; i < 4; ++i)
                n |= static_cast<u32>(b_[at_ + static_cast<size_t>(i)]) << (8 * i);
            at_ += 4;
            if (at_ + n > b_.size()) return false;
            lua_pushlstring(L, reinterpret_cast<const char*>(b_.data() + at_), n);
            at_ += n;
            return true;
        }
        case kNil: lua_pushnil(L); return true;
        case kBool:
            if (at_ >= b_.size()) return false;
            lua_pushboolean(L, b_[at_++] != 0);
            return true;
        case kTable: {
            if (!lua_checkstack(L, 4)) return false; // the table, a key, a value
            lua_newtable(L);
            const int t = lua_gettop(L);
            for (;;) {
                if (at_ >= b_.size()) {
                    lua_settop(L, t - 1);
                    return false;
                }
                if (b_[at_] == kEnd) {
                    ++at_;
                    return true;
                }
                if (!value(L, depth + 1) || !value(L, depth + 1)) {
                    lua_settop(L, t - 1);
                    return false;
                }
                // A nil key (a table key that couldn't travel) or value: skipped
                if (lua_isnil(L, -2) || lua_isnil(L, -1)) lua_pop(L, 2);
                else lua_rawset(L, t);
            }
        }
        default: return false;
        }
    }
    bool done() const { return at_ == b_.size(); }

private:
    const std::vector<u8>& b_;
    size_t at_ = 0;
};

} // namespace

std::vector<u8> encode_lobby_value(lua_State* L, int idx) {
    std::vector<u8> out;
    std::vector<const void*> open;
    encode(L, idx, 0, open, out);
    return out;
}

bool push_lobby_value(lua_State* L, const std::vector<u8>& bytes) {
    Decoder d(bytes);
    if (!d.value(L, 0)) {
        lua_pushnil(L);
        return false;
    }
    if (!d.done()) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return false;
    }
    return true;
}

} // namespace osc::lua
