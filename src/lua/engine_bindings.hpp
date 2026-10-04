#pragma once

struct lua_State;
using lua_CFunction = int (*)(lua_State*);

namespace osc::lua {

class LuaState;

/// Register all engine bindings needed for the init context
/// (before VFS is constructed).
void register_init_bindings(LuaState& state);

/// Register all engine bindings needed for the blueprint loading context
/// (after VFS is constructed).
void register_blueprint_bindings(LuaState& state);

/// Whether `f` is the blueprint phase's ForkThread or KillThread: stand-ins a
/// sim or UI state must replace with its thread manager's
bool is_thread_stand_in(lua_CFunction f);

} // namespace osc::lua
