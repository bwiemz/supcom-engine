#pragma once

// A winged aircraft's attack runs, as Moho flies them: the combat state
// machine (CUnitMotion::ComputeAirCombatTactics), the airframe's lag in a
// run, and where a bomb must leave to land on its target (CalcBombDrop,
// UnitWeapon::CanFire). docs/plans/2026-10-05-air-attack-runs-design.md.
// And a hovering aircraft's circling (CalcCirclingOrientation; roadmap
// item 6).

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
    /// The most of the speed its thrust may take (a guard's slowing within
    /// StartTurnDistance; 1 otherwise)
    f32 thrust_cap = 1.0f;
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
/// The same at a point on the ground: a ground attack order's runs.
void fly_attack_run(Unit& unit, const Vector3& at, SimState& sim, const map::Terrain* terrain,
                    f32 dt);

/// One tick of a flight that has the airframe (an attack run, a guard's
/// winged flight): the nose and velocity follow `steer` with Moho's lag
/// (KTurn, KTurnDamping, KMove), the height its climb rate toward
/// steer.altitude.
void fly_airframe(Unit& unit, const AirSteer& steer, SimState& sim, const map::Terrain* terrain,
                  f32 dt);

/// The run ended: its combat state goes back to None, and the airframe to
/// the plain flight, at the speed it had.
void end_attack_run(Unit& unit);

/// A hovering aircraft that circles rather than hangs still: a flier in the
/// air, not winged and not HoverOverAttack (Moho's ComputeAirControl; its
/// target or its work decide when), and not flying winged on guard.
bool circles(const Unit& unit);

/// A hovering aircraft (not Air.Winged) on a guard order with nothing to do
/// -- not attacking, building, repairing or ferrying -- flies as a winged
/// one (Moho's ComputeAirControl, 0x006BE6B0: the hover family only
/// `!Guarding || Moving || Ferrying || Attacking || Building || Repairing`).
/// Reclaim and capture don't count. HoverOverAttack doesn't spare it.
bool flies_winged_on_guard(const Unit& unit);

/// Its flight then (CalcWingedOrientation, no combat state, guarding): the
/// nose turned toward `goal`, the thrust along the nose at the lesser of
/// the distance and its top speed, scaled by how well the nose points
/// (at least half), and within StartTurnDistance by at most
/// distance / StartTurnDistance, never past half -- so it slows onto its
/// goal and comes to rest there (CalcMoveAir's Stopping and Stopped, with
/// no combat state). It never circles.
void fly_winged_to(Unit& unit, const Vector3& goal, SimState& sim, const map::Terrain* terrain,
                   f32 dt);

/// Moho's re-pick in CalcCirclingOrientation, once `tick` is past the
/// state's timeout. In order, it draws:
/// 1. the way round (only with CirclingDirChange);
/// 2. the height off AttackElevation, within +-CirclingElevationChangeRatio
///    of it;
/// 3. the radius's ratio;
/// 4. the next timeout, CirclingFlightChangeFrequency to twice that on.
void circling_draws(AirCombatState& state, const AirCombatRules& rules, f32 attack_elevation,
                    u32 tick, SimRandom& rng);

/// The geometry one circling tick looks at.
struct CirclingInput {
    Vector3 position;  ///< the aircraft's
    Vector3 center;    ///< what it circles
    f32 radius = 0.0f; ///< its circle's (the ratio applied)
    f32 min_airspeed = 0.0f;
    f32 max_airspeed = 0.0f;
    f32 height = 0.0f; ///< AttackElevation plus the drawn offset
    bool reverse = false;
};

/// What the circling asks this tick (CalcCirclingOrientation).
struct CirclingSteer {
    Vector3 aim;       ///< the point on the circle it makes for
    Vector3 velocity;  ///< per second: to the aim, at most MaxAirspeed
    f32 facing = 0.0f; ///< its nose, at the centre: atan2(x, z)
};

/// Moho's aim: the circle's point a MinAirspeed's step round from where
/// the aircraft is, at its height over the terrain there (the terrain only,
/// at the nearest whole coordinates). Its velocity heads there, in three
/// dimensions.
CirclingSteer circling_steer(const CirclingInput& in, const map::Terrain* terrain);

/// What a circling aircraft circles.
struct CircleAround {
    Vector3 center;
    /// Its target weapon's MaxRadius. 0: none, and the circle is
    /// StartTurnDistance's.
    f32 weapon_radius = 0.0f;
    bool target_in_air = false; ///< CirclingRadiusVsAirMult applies
    /// Where its move order would take it (Moho's mTargetPosition), which
    /// the airframe's damping reads.
    Vector3 move_goal;
};

/// One tick of a hovering aircraft circling: the re-pick, the aim, and the
/// airframe after it with Moho's lag. The nose turns to the centre under
/// KTurn x CirclingTurnMult, and the velocity eases under KMove and the
/// damping. It leans into its change of velocity by BankFactor.
void fly_circling(Unit& unit, const CircleAround& around, SimState& sim,
                  const map::Terrain* terrain, f32 dt);

} // namespace osc::sim
