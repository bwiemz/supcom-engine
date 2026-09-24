#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <string>
#include <vector>

namespace osc::map {
class Pathfinder;
class Terrain;
}

namespace osc::sim {
class SimState;
class Unit;

class Navigator {
public:
    /// Idle: no goal. Moving: following a path. WaitingForPath: the per-tick
    /// pathfinding budget was spent; the goal is kept and callers retry
    /// set_goal next tick (see is_moving()).
    enum class Status : u8 { Idle, Moving, WaitingForPath };

    /// Set goal with A* pathfinding (preferred).
    void set_goal(const Vector3& pos, const map::Pathfinder* pathfinder,
                  const Vector3& current_pos, const std::string& layer,
                  f32 draft = 0, bool amphibious = false);

    /// Set goal with straight-line movement (legacy/fallback).
    void set_goal(const Vector3& pos);

    void abort_move();

    const Vector3& goal() const { return goal_; }
    Status status() const { return status_; }
    /// True only while following a path. False while waiting for one, so the
    /// usual `if (!is_moving() || goal changed) set_goal(...)` retries it.
    bool is_moving() const { return status_ == Status::Moving; }
    /// True while a move is in progress or pending a path -- i.e. the goal
    /// has not been reached. Use this, not is_moving(), to detect arrival.
    bool busy() const { return status_ != Status::Idle; }

    /// Move the unit toward its goal at up to `max_speed`. Returns true until
    /// the goal is reached (including while waiting for a throttled path,
    /// without moving). If terrain is provided, sets its Y to the surface.
    /// A surface unit drives (see drive()); an aircraft here goes straight.
    bool update(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain = nullptr);

    /// Air-specific movement: heading-based steering, acceleration, altitude management.
    /// Reads/writes air state on the Unit. Returns true if still moving.
    bool update_air(Unit& unit, f64 dt,
                    const map::Terrain* terrain = nullptr);

    bool speed_through_goal() const { return speed_through_goal_; }
    void set_speed_through_goal(bool b) { speed_through_goal_ = b; }

    void set_sim_state(const SimState* sim) { sim_ = sim; }

    /// After a path request fails outright (nothing reachable is closer to
    /// the goal), identical requests -- same goal, unit still where it was --
    /// return without searching, except every Nth call, since a blocking
    /// structure may have died meanwhile. Chase-style command handlers
    /// re-request every tick while not moving; without this a unit parked at
    /// the closest point to an unreachable target ran a full A* each tick and
    /// could starve the shared per-tick path budget.
    static constexpr int FAILED_PATH_RETRY_CALLS = 50;

    /// A goal behind it this far off its heading is backed up to, if near.
    static constexpr f32 REVERSE_ANGLE = 1.5707964f; // 90 degrees
    /// A unit that can pivot does so once its goal is this far off its heading.
    static constexpr f32 PIVOT_ANGLE = 0.7853982f; // 45 degrees
    /// One that can't keeps this share of its speed to turn.
    static constexpr f32 TURNING_SPEED = 0.3f;

private:
    /// A surface unit's drive: turn toward the next waypoint at its
    /// TurnRate (faster at speed where TurnRadius allows), and drive along
    /// its heading, accelerating at MaxAcceleration, slowing to turn,
    /// pivoting if it can, and braking at MaxBrake to stop on its goal. A
    /// goal just behind it within BackUpDistance it backs up to.
    bool drive(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain);
    /// Straight at the waypoint at full speed: aircraft on orders that don't
    /// fly them (the flight model is update_air).
    bool slide(Entity& entity, f32 max_speed, f64 dt, const map::Terrain* terrain);
    /// Reached the goal: no path left.
    void arrive();

    const SimState* sim_ = nullptr;
    Vector3 goal_;
    Status status_ = Status::Idle;
    bool speed_through_goal_ = false;
    std::vector<Vector3> waypoints_;
    size_t waypoint_index_ = 0;

    // Memo of the last outright path failure (see FAILED_PATH_RETRY_CALLS).
    bool has_failed_request_ = false;
    Vector3 failed_goal_;
    Vector3 failed_from_;
    int suppressed_requests_ = 0;
    static constexpr f32 ARRIVAL_TOLERANCE = 0.5f;
    static constexpr f32 WAYPOINT_TOLERANCE = 1.5f;
};

} // namespace osc::sim
