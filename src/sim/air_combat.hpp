#pragma once

// A winged aircraft's attack runs, as Moho flies them: the combat state
// machine (CUnitMotion::ComputeAirCombatTactics), the airframe's lag in a
// run, and where a bomb must leave to land on its target (CalcBombDrop,
// UnitWeapon::CanFire). docs/plans/2026-10-05-air-attack-runs-design.md.

#include "core/types.hpp"
#include "sim/unit.hpp"

#include <optional>

namespace osc::map {
class Terrain;
}

namespace osc::sim {

class SimState;
class SimRandom;

/// Moho's EAirCombatState.
enum class AirCombatMode : u8 {
    None = 0,
    Combat = 1,
    NormalTurn = 2,
    CombatTurn = 3,
    CombatTurnB = 4,
    Realign = 5,
    BreakOff = 6,
    ReturnToMap = 7,
};

/// What a run asks of the airframe this tick.
struct AirSteer {
    f32 heading = 0.0f;       ///< the way to fly, atan2(x, z)
    f32 speed = 0.0f;         ///< units a second
    f32 turn_clamp = 0.0f;    ///< how far the nose may be led, radians
    f32 turn_gain = 0.0f;     ///< KTurn, raised in a run and a combat turn
    f32 altitude = 0.0f;      ///< the height to hold, absolute
    bool full_thrust = false; ///< past NormalTurn: thrust doesn't wait for the nose
    bool making_attack_run = false;
};

/// The geometry a tactics step looks at (so the rules can be tested alone).
struct AirTacticsInput {
    Vector3 position;   ///< the aircraft's
    f32 heading = 0.0f; ///< its nose, atan2(x, z)
    Vector3 target;     ///< the target's position
    bool target_in_air = false;
    bool target_moved = false; ///< it moved over its last tick
    f32 target_heading = 0.0f; ///< its nose (for an aircraft)
    bool in_map = true;        ///< the aircraft is in the playable area
    bool in_map_by_5 = true;   ///< ... and 5 inside its edge
    u32 tick = 0;
    f32 max_airspeed = 0.0f;       ///< MaxAirspeed x its speed multiplier
    f32 break_off_trigger = 0.0f;  ///< x its script's multiplier
    f32 break_off_distance = 0.0f; ///< x its script's multiplier
};

/// One tick of Moho's ComputeAirCombatTactics: the next combat state, its
/// timeout and the turning count, drawing from `rng` as Moho draws. The
/// result is in `state`; rules are the aircraft's blueprint's.
void air_tactics(AirCombatState& state, const AirCombatRules& rules, const AirTacticsInput& in,
                 SimRandom& rng);

/// Moho's CalcBombDrop: where the aircraft must be for a bomb dropped now
/// with its velocity (per second) to land on `target`, falling at
/// `gravity`; none when no fall reaches it.
std::optional<Vector3> calc_bomb_drop(const Vector3& velocity, const Vector3& position,
                                      const Vector3& target, f32 gravity);

/// Moho's bomb release gate (UnitWeapon::CanFire, winged): at the release
/// point, within `threshold` (BombDropThreshold); half that out, only with
/// the point behind or abeam and the target still ahead. faf-re's text has
/// the two outer answers swapped, which drops long before the point; this is
/// the reading under which the bombs land on the target (the design's §3.3).
bool bomb_release_ok(const Vector3& position, f32 heading, const Vector3& release,
                     const Vector3& target, f32 threshold);

/// Moho's PredictAheadBomb for a unit that doesn't spin: where `target`
/// will be in `seconds` at its last tick's velocity.
Vector3 predict_ahead(const Entity& target, f32 seconds);

/// A winged aircraft attacking `target` this tick: its tactics, then its
/// flight in the run (the airframe's lag, its height over the target's
/// ground). Sets or clears its MakingAttackRun state.
void fly_attack_run(Unit& unit, const Entity& target, SimState& sim, const map::Terrain* terrain,
                    f32 dt);

/// The run ended: its combat state goes back to None, and the airframe to
/// the plain flight, at the speed it had.
void end_attack_run(Unit& unit);

} // namespace osc::sim
