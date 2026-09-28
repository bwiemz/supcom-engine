#pragma once

// A lobby message's Lua data (M218a): what SendData and BroadcastData send,
// as bytes, and back. The tags are Moho's (LuaObject's lobby serializer):
// 0 number, 1 string, 2 nil, 3 boolean, 4 a table's pairs, 5 its end.
// Numbers travel as doubles where Moho's are floats: exact, and the engine
// talks only to itself.

#include "core/types.hpp"

#include <vector>

struct lua_State;

namespace osc::lua {

/// Tables deeper than this travel as nil there (as a table holding itself
/// does, and functions, userdata and threads anywhere).
inline constexpr int kLobbyWireMaxDepth = 32;

/// The value at `idx`, encoded.
std::vector<u8> encode_lobby_value(lua_State* L, int idx);

/// Push the value `bytes` hold. False, pushing nil, if they are malformed.
bool push_lobby_value(lua_State* L, const std::vector<u8>& bytes);

} // namespace osc::lua
