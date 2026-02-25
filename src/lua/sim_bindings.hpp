#pragma once

namespace osc::sim {
class SimState;
}

namespace osc::lua {

class LuaState;

/// Register simulation global functions (_c_CreateEntity, CreateUnit,
/// ForkThread, categories, and many stubs). Must be called before the sim
/// Lua environment boots.
void register_sim_bindings(LuaState& state, sim::SimState& sim);

} // namespace osc::lua
