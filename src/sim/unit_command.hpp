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
    BuildMobile = 20,     // engineer builds structure/unit at position
    BuildFactory = 21,    // factory produces unit at own position
    Reclaim = 25,         // engineer reclaims prop/wreckage/unit
    Repair = 30,          // engineer repairs damaged unit
    Upgrade = 35,         // structure upgrades to next tier
    Capture = 40,         // engineer captures enemy unit
    Dive = 45,            // submarine submerge/surface toggle
    Enhance = 50,         // ACU/SACU self-enhancement (same unit)
    TransportLoad = 60,   // ground unit → load into transport (target_id = transport)
    TransportUnload = 61, // transport → unload all cargo at position
    Nuke = 70,            // launch a nuke at a position (M206)
    Tactical = 71,        // launch a tactical missile at a unit or position
    Overcharge = 72,      // overcharge attack (ACU ability)
    Sacrifice = 73,       // sacrifice unit to speed up construction
    Teleport = 74,        // teleport to target position
    Ferry = 75,           // ferry route waypoint (transport loop)
    // Silo builds (IssueSiloBuildNuke/Tactical): applied to the unit's silo,
    // not queued, so ordering a missile does not cancel what it is doing.
    SiloBuildNuke = 76,
    SiloBuildTactical = 77,
};

struct UnitCommand {
    CommandType type = CommandType::Stop;
    Vector3 target_pos;
    u32 target_id = 0;          // entity ID for Attack/Guard
    std::string blueprint_id;   // for Build commands (empty for non-build)
    u32 command_id = 0;         // unique ID for IsCommandsActive tracking
    /// A group order's formation (a /lua/formations.lua function, e.g.
    /// AttackFormation): the sim lays its units out in slots about the target
    /// when it applies the order (M204). Empty: every unit to the target.
    std::string formation;
    /// The formation's facing, as IssueFormMove's degrees give it (south 0,
    /// east 90: the engine's heading). Unset: from the group to the target.
    bool has_facing = false;
    f32 facing = 0;
    /// Held to this speed (a formation keeps its slowest unit's pace); 0:
    /// the unit's own. Set when a formation order is laid out.
    f32 speed_cap = 0;
    /// A launch order's weapon has fired for it (runtime state: not sent
    /// with the order). The order then ends.
    bool launched = false;
    /// The order has handed its unit to the script (a teleport's
    /// OnTeleportUnit, an OverCharge's OnEnableWeapon); runtime state.
    bool started = false;
    /// Out of reach, the unit was sent just clear of its target, and works
    /// from where it arrives or gives up (build, repair, reclaim, capture;
    /// see sim/work_range.hpp); runtime state.
    bool approached = false;
    /// A build's site skirt, looked up once from its blueprint (runtime
    /// state; 0 until then).
    f32 site_skirt_x = 0;
    f32 site_skirt_z = 0;
    /// A launch order's target is within its weapon's range band this tick:
    /// only then does the weapon take it, as Moho's fire-at task hands the
    /// weapon its target (runtime state).
    bool in_band = false;
};

} // namespace osc::sim
