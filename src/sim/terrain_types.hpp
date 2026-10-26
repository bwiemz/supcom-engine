#pragma once

// /lua/TerrainTypes.lua's entries, by TypeCode, from the sim's Lua state: what
// GetTerrainType answers its scripts, and what a unit's OnTerrainTypeChange
// hears (Moho's STIMap::GetTerrainType).

#include "core/types.hpp"

struct lua_State;

namespace osc::sim {

/// Push the entry for TypeCode `code`: the one TerrainTypes.lua lists for it,
/// else its 'Default' (code 1), else, without FA's scripts, a table naming
/// 'Default'.
void push_terrain_type(lua_State* L, u8 code);

} // namespace osc::sim
