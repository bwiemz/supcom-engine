#pragma once

// The player's options as the engine reads them: Moho's OPTIONS_GetInt,
// OPTIONS_GetFloat, OPTIONS_GetString and OPTIONS_GetBool (faf-re
// app/StartupClientHelpers.cpp) ask the UI state's /lua/user/prefs.lua
// GetOption(key) -- the current profile's option, else options.lua's
// default -- and convert its answer as LuaPlus does. When the call fails
// (no module, no function, a script error) they warn "Error running
// '/lua/user/prefs.lua:GetOption' : <error>" and answer 0, 0, "" or false.
// Each asks again: nothing is cached.

#include "core/types.hpp"

#include <string>

struct lua_State;

namespace osc::lua {

/// The answer as a number, truncated (0 for one that isn't).
int options_get_int(lua_State* L, const char* key);
/// The answer as a number (0 for one that isn't).
f32 options_get_float(lua_State* L, const char* key);
/// The answer as a string ("" for one that is neither a string nor a
/// number).
std::string options_get_string(lua_State* L, const char* key);
/// The answer's truth: false only for nil and false.
bool options_get_bool(lua_State* L, const char* key);

} // namespace osc::lua
