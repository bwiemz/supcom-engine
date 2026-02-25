#pragma once

struct lua_State;

namespace osc::sim {
class SimState;
}

namespace osc::lua {

class LuaState;

/// Populate the `moho` global table with all class method tables.
/// Must be called before globalInit.lua executes.
void register_moho_bindings(LuaState& state, sim::SimState& sim);

} // namespace osc::lua
