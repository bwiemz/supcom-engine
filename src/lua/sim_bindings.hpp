#pragma once

#include "core/types.hpp"

#include <string>

struct lua_State;

namespace osc::sim {
class SimState;
struct PlacementRules;
}

namespace osc::lua {

class LuaState;

/// Register simulation global functions (_c_CreateEntity, CreateUnit,
/// ForkThread, categories, and many stubs). Must be called before the sim
/// Lua environment boots.
void register_sim_bindings(LuaState& state, sim::SimState& sim);
/// Moho's Core functions both Lua states have (FAF's engine/Core.lua): the
/// vector math and the alliance queries. The UI's scripts use them too
/// (FAF's utilities.lua, imported by its orders and key actions, needs VDist3).
void register_core_bindings(LuaState& state);

/// Register CreatePrefetchSet (a loading hint; the prefetcher is a no-op)
/// on a state that has no sim bindings, e.g. the UI state for userInit.lua.
void register_prefetch_bindings(LuaState& state);

/// Register the `categories` global (lazily created category objects with
/// + - * set operators) and ParseEntityCategory on a state without sim
/// bindings. Moho gives the UI
/// state one too: the game UI's range overlays, selection helpers and
/// construction tabs build category expressions.
void register_category_bindings(LuaState& state);

/// Push the sim state's shared Vector metatable (x/y/z alias [1]/[2]/[3]),
/// creating it on first use. Every vector the engine hands to scripts
/// carries it: retail reads positions as both pos[1] and pos.x.
void push_vector_metatable(lua_State* L);

void set_manip_metatable(lua_State* L, int table_idx, const char* cache_key,
                         const char* moho_class_name);

/// Whether army `army` may build structure `bp_id` centred at (x, z), by
/// StructurePlacement with the orders not yet run
bool can_build_structure(lua_State* L, const sim::SimState& sim, int army, const std::string& bp_id,
                         f32 x, f32 z);

/// A structure blueprint's placement rules, as its Physics give them (the
/// sim keeps them once read)
const sim::PlacementRules& structure_rules(lua_State* L, const sim::SimState& sim,
                                           const std::string& bp_id);

} // namespace osc::lua
