#pragma once

#include "core/result.hpp"

#include <string_view>

struct lua_State;

namespace osc::lua {

/// Run a script from the VFS the way the engine's `doscript` does: the file
/// itself, then every `<hook dir><path>` that exists (e.g. /schook/lua/
/// simInit.lua after /lua/simInit.lua), all in one environment.
///
/// `env_index` is a stack index of the environment table, or 0 for the
/// globals table. The VFS comes from the state's registry (LuaState::set_vfs).
/// On failure returns an error naming the script; the Lua stack is left as
/// it was either way.
///
/// Every engine entry point that executes game scripts must go through this
/// (not LuaState::do_buffer), or init hooks silently stop applying.
Result<void> run_vfs_script(lua_State* L, std::string_view path, int env_index = 0);

} // namespace osc::lua
