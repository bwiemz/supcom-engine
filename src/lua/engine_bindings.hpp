#pragma once

struct lua_State;

namespace osc::lua {

class LuaState;

/// Register all engine bindings needed for the init context
/// (before VFS is constructed).
void register_init_bindings(LuaState& state);

/// Register all engine bindings needed for the blueprint loading context
/// (after VFS is constructed).
void register_blueprint_bindings(LuaState& state);

} // namespace osc::lua
