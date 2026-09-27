#pragma once

#include "core/types.hpp"

#include <vector>

struct lua_State;

namespace osc::sim {

/// FA's colour tables, /lua/GameColors.lua's GameColors, decoded as Moho
/// decodes them (packed ARGB).
struct GameColors {
    /// ArmyColors: a lobby slot's colour index names one.
    std::vector<u32> army_colors;
    /// PlayerColors: as many as a mesh's lookup texture has rows.
    std::vector<u32> player_colors;
    /// UnidentifiedColor: a blip's icon before its unit has been seen
    /// (Moho's GetUnidentifiedColor; M215a).
    u32 unidentified_color = 0xFF808080u;
};

/// Read GameColors through the state's import(); lists it can't read are
/// empty.
GameColors read_game_colors(lua_State* L);

} // namespace osc::sim
