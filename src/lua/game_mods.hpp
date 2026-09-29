#pragma once

// A game's mods in its Lua states (M221b): Moho's rules state sets
// __active_mods from the launch's GameMods before RuleInit.lua loads the
// blueprints, and copies it into the sim's state and the session's UI state.

#include <string>

struct lua_State;

namespace osc::lua {

/// Set L's __active_mods to a game's mods (sim::GameSetup::mods): the list,
/// or an empty table when there is none -- or when `mods` holds no table,
/// which is reported.
void set_active_mods(lua_State* L, const std::string& mods);

/// Give L an empty __active_mods unless it has one: what the bindings that
/// scripts expect it from do, so as not to replace a launch's.
void ensure_active_mods(lua_State* L);

/// Note that the UI state L ran SessionInit.lua, which loads UserSync.lua
/// itself: the world UI then doesn't load it again.
void mark_session_init(lua_State* L);
bool session_init_ran(lua_State* L);

} // namespace osc::lua
