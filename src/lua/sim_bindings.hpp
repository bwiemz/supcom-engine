#pragma once

struct lua_State;

namespace osc::sim {
class SimState;
}

namespace osc::lua {

class LuaState;

/// Register simulation global functions (_c_CreateEntity, CreateUnit,
/// ForkThread, categories, and many stubs). Must be called before the sim
/// Lua environment boots.
void register_sim_bindings(LuaState& state, sim::SimState& sim);

/// Register CreatePrefetchSet (a loading hint; the prefetcher is a no-op)
/// on a state that has no sim bindings, e.g. the UI state for userInit.lua.
void register_prefetch_bindings(LuaState& state);

/// Push the sim state's shared Vector metatable (x/y/z alias [1]/[2]/[3]),
/// creating it on first use. Every vector the engine hands to scripts
/// carries it: retail reads positions as both pos[1] and pos.x.
void push_vector_metatable(lua_State* L);

} // namespace osc::lua
