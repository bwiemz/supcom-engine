#pragma once

// A Lua state's sim: the sim's own state holds it, and the UI's while a game
// runs, in the state's registry under one key (sim_state.cpp). The bindings
// and the sim read it through get_sim.

struct lua_State;

namespace osc::sim {

class SimState;

/// Make `sim` the sim `L` holds; null clears it.
void set_sim(lua_State* L, SimState* sim);
/// The sim `L` holds, or null when it holds none.
SimState* get_sim(lua_State* L);

} // namespace osc::sim
