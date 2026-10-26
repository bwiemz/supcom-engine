#pragma once

#include "sim/resource_deposit.hpp"

#include <span>
#include <string>
#include <string_view>

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

/// STR_Utf8SubString(s, start, count): `count` characters from the
/// `start`th, counted from 1, as maui/text.lua's WrapText splits a word
std::string utf8_substring(std::string_view s, int start, int count);
/// GetDepositsAroundPoint(X, Z, Radius, Type) over `deposits`, in the sim
/// or the UI (FAF's SimGetDepositsAroundPoint patch): a list of those of
/// Type (1 mass, 2 hydrocarbon; 0 any) whose square's centre is within
/// Radius of (X, Z), in the map's order, each {X1, Z1, X2, Z2, Type, Dist};
/// nothing for a NaN argument. Returns its Lua result count (0 or 1).
int push_deposits_around_point(lua_State* L, std::span<const sim::ResourceDeposit> deposits);
/// Whether `f` is the blueprint phase's ForkThread or KillThread: stand-ins a
/// sim or UI state must replace with its thread manager's
bool is_thread_stand_in(lua_CFunction f);

} // namespace osc::lua
