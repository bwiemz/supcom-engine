#pragma once

struct lua_State;

namespace osc::lua {

class LuaState;

/// Register the Register*Blueprint and SpecFootprints C functions into Lua.
void register_blueprint_store_bindings(LuaState& state);

} // namespace osc::lua
