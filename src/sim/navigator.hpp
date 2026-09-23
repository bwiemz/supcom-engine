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

    /// Move entity toward goal. Returns true until the goal is reached
    /// (including while waiting for a throttled path, without moving).
    /// If terrain is provided, sets entity Y to surface height.
    bool update(Entity& entity, f32 max_speed, f64 dt,
                const map::Terrain* terrain = nullptr);

    /// Air-specific movement: heading-based steering, acceleration, altitude management.
    /// Reads/writes air state on the Unit. Returns true if still moving.
    bool update_air(Unit& unit, f64 dt,
                    const map::Terrain* terrain = nullptr);

    bool speed_through_goal() const { return speed_through_goal_; }
    void set_speed_through_goal(bool b) { speed_through_goal_ = b; }

    void set_sim_state(const SimState* sim) { sim_ = sim; }

private:
    const SimState* sim_ = nullptr;
    Vector3 goal_;
    Status status_ = Status::Idle;
    bool speed_through_goal_ = false;
    std::vector<Vector3> waypoints_;
    size_t waypoint_index_ = 0;
    static constexpr f32 ARRIVAL_TOLERANCE = 0.5f;
    static constexpr f32 WAYPOINT_TOLERANCE = 1.5f;
};

} // namespace osc::sim
