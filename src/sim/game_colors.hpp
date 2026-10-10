#pragma once

#include "core/types.hpp"

#include <vector>

struct lua_State;

namespace osc::sim {

/// GameColors.TeamColorMode.
struct TeamColors {
    u32 self = 0xFF4169E1u;
    u32 ally = 0xFF006400u;
    u32 enemy = 0xFFE80A0Au;
    u32 neutral = 0xFFDAA520u;
};

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
    /// CivilianArmyColor: a civilian army's (Moho's GetCivilianArmyColor).
    u32 civilian_army_color = 0xFFDEB887u;
    TeamColors team_colors;
};

/// Read GameColors through the state's import(); lists it can't read are
/// empty.
GameColors read_game_colors(lua_State* L);

} // namespace osc::sim
