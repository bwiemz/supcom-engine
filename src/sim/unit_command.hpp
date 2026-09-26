#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <string>
#include <vector>

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
    TransportUnload = 61, // transport → unload its cargo (or unload_ids) at position
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
    // A land unit waiting at a ferry beacon to be carried (IssueTransportLoad
    // at a beacon; Moho's CUnitWaitForFerryTask). target_id = the beacon.
    WaitForFerry = 78,
    // An aircraft docks at an air staging platform to refuel and repair
    // (the orders panel's Dock; Moho's UNITCOMMAND_Dock, M206r). target_id =
    // the platform. A TransportLoad onto a platform docks the same way.
    Dock = 79,
};

/// Where a refuel order is (Moho's CUnitRefuel task states, M206r).
enum class DockPhase : u8 {
    Reserve,  ///< asking the platform for a slot, heading for it meanwhile
    Approach, ///< flying to the slot's bone, down to it, turning to its facing
    Docked,   ///< attached: refuelling and repairing until full
    Lift,     ///< released: climbing back to its flying height
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
    /// A TransportUnload's cargo to drop, chosen when the order is issued
    /// (IssueTransportUnloadSpecific's category, as Moho's
    /// UNITCOMMAND_TransportUnloadSpecificUnits carries its unit set). The
    /// rest stays aboard. Empty: all of it.
    std::vector<u32> unload_ids;
    /// A factory command (Moho's IssueFactoryCommand, M206k): it goes to the
    /// units' rally orders, not their queues, and a fresh one clears those.
    /// A player's move, patrol or transport call to a selected factory.
    bool factory = false;
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
    /// A ferry route's first order: the beacon it loads at (runtime state; 0
    /// until the route starts).
    u32 beacon_id = 0;
    /// A WaitForFerry order: the ferry that took the unit, which it boards
    /// (runtime state; 0 while it waits).
    u32 assigned_id = 0;
    /// A factory build whose unit is done: ticks until it next looks at
    /// whether the factory is still busy rolling the unit off (runtime
    /// state; 0 while it builds). See Unit::order_build_in_place.
    i32 rolloff_wait = 0;
    /// A carrier's unload that launches its stored units (M206q; runtime
    /// state): those still to go, and ticks until the next leaves (-1: the
    /// launch has not started).
    std::vector<u32> launch_queue;
    i32 launch_wait = -1;
    /// A refuel order (M206r; runtime state): its phase, and ticks until it
    /// next looks (Moho's task waits: 9 ticks between asks for a slot and
    /// between checks of a docked unit's tank).
    DockPhase dock_phase = DockPhase::Reserve;
    i32 dock_wait = 0;
    /// A refuel a patrol broke off for (Moho's patrol task handing its unit
    /// to a refuel task): it gives up, rather than waits, when there is no
    /// room, and ends without waiting for others. Runtime state.
    bool patrol_refuel = false;
    /// A patrol's ticks to wait before it next looks for a platform to refuel
    /// at (Moho's patrol task runs every 6 ticks). Runtime state.
    i32 patrol_scan = 0;
};

} // namespace osc::sim
