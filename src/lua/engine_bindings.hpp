#pragma once

#include <string>
#include <string_view>

struct lua_State;

namespace osc::lua {

class LuaState;

/// Register all engine bindings needed for the init context
/// (before VFS is constructed).
void register_init_bindings(LuaState& state);

/// Register all engine bindings needed for the blueprint loading context
/// (after VFS is constructed).
void register_blueprint_bindings(LuaState& state);

/// STR_Utf8SubString(s, start, count): `count` characters from the
/// `start`th, counted from 1, as maui/text.lua's WrapText splits a word
std::string utf8_substring(std::string_view s, int start, int count);

} // namespace osc::lua
