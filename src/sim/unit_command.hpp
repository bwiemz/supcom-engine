#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <string>

namespace osc::sim {

enum class CommandType : u8 {
    Stop = 1,
    Move = 2,
    Attack = 10,
    Guard = 15,
    Patrol = 16,
    BuildMobile = 20,  // engineer builds structure/unit at position
    BuildFactory = 21, // factory produces unit at own position
    Reclaim = 25,      // engineer reclaims prop/wreckage/unit
    Repair = 30,       // engineer repairs damaged unit
    Upgrade = 35,      // structure upgrades to next tier
    Capture = 40,      // engineer captures enemy unit
    Dive = 45,         // submarine submerge/surface toggle
    Enhance = 50,      // ACU/SACU self-enhancement (same unit)
    TransportLoad = 60,    // ground unit → load into transport (target_id = transport)
    TransportUnload = 61,  // transport → unload all cargo at position
};

struct UnitCommand {
    CommandType type;
    Vector3 target_pos;
    u32 target_id = 0;          // entity ID for Attack/Guard
    std::string blueprint_id;   // for Build commands (empty for non-build)
    u32 command_id = 0;         // unique ID for IsCommandsActive tracking
};

} // namespace osc::sim
