#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3
#include "sim/path_navigator.hpp"

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
    friend struct StateIO; // snapshots (state_io.hpp)
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

    void set_sim_state(SimState* sim) { sim_ = sim; }

    /// Following, or about to follow, a path as Moho's units do (roadmap
    /// item 4c-2c; SimState::moho_pathing).
    bool moho_active() const { return moho_active_ || moho_pending_; }
    const path::PathNavigator& moho_path() const { return moho_; }

    // Steering (M203c; Moho's CAiSteeringImpl, see steering.hpp).
    /// A meeting the unit expects on the path ahead (Moho's
    /// COLLISIONTYPE_1): the unit it would meet, the tick and the spot.
    struct Collision {
        u32 other = 0;
        i32 tick = 0;
        Vector3 at{};
    };
    const Collision& collision() const { return collision_; }
    bool has_collision() const { return collision_.other != 0; }
    void set_collision(const Collision& c) { collision_ = c; }
    void clear_collision() { collision_ = {}; }
    /// The tick its next check is due (0: now). A new path is checked at
    /// once, then again as the path ahead runs out.
    i32 next_check() const { return next_check_; }
    void set_next_check(i32 tick) { next_check_ = tick; }
    /// Where it expects to be each of the next `nodes` ticks: its waypoints
    /// sampled from `from` at its speed, rising toward `top_speed` at
    /// `accel` a second (Moho's spline nodes, without their turns). Empty
    /// when it has no path.
    void path_ahead(const Vector3& from, f32 speed, f32 top_speed, f32 accel, int nodes,
                    std::vector<Vector3>& out) const;
    /// Step aside to overtake: drive to `point` first, then on (Moho's PT_2
    /// path).
    void sidestep(const Vector3& point);
    bool sidestepping() const { return sidestep_ && waypoint_index_ <= sidestep_index_; }
    /// Stop for unit `for_id` crossing or coming head-on (Moho's mode 4
    /// path): brake at twice its brake, then wait `ticks` once still.
    void hold(int ticks, u32 for_id);
    bool holding() const { return hold_ticks_ > 0; }
    /// The unit it is stopped for (while holding).
    u32 held_for() const { return hold_ticks_ > 0 ? held_for_ : 0; }

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
    /// Jostled this many ticks near its goal without getting nearer, a unit
    /// in a crowd counts itself there (others hold the spot).
    static constexpr int CROWD_TICKS = 5;

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
    /// No meeting expected, nothing under way: a new path's steering.
    void reset_steering();
    /// A ground unit's move as Moho's CAiNavigatorLand::Execute runs it: its
    /// path navigator updated, then the drive at its target cell.
    bool update_moho(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain);
    /// Drop the Moho path, if any.
    void reset_moho();
    /// The playable area (the whole map without one, or for an army that may
    /// go anywhere when `whole_map_if_allowed`).
    OccupancyRect moho_bounds(const Unit& unit, bool whole_map_if_allowed = true) const;
    /// The goal by the grid pathfinder (set_goal's own way, and Moho
    /// pathing's for a unit with no footprint class).
    void set_goal_grid(const Vector3& pos, const map::Pathfinder* pathfinder,
                       const Vector3& current_pos, const std::string& layer, f32 draft,
                       bool amphibious);

    SimState* sim_ = nullptr;
    Vector3 goal_;
    Status status_ = Status::Idle;
    bool speed_through_goal_ = false;
    std::vector<Vector3> waypoints_;
    size_t waypoint_index_ = 0;
    // Progress toward the goal, for arriving in a crowd (see CROWD_TICKS).
    f32 best_dist_ = 1e30f;
    int stalled_ = 0;

    // Steering (M203c).
    Collision collision_;
    i32 next_check_ = 0;
    bool sidestep_ = false;     ///< a sidestep point is on the path
    size_t sidestep_index_ = 0; ///< its index; passed, the sidestep is done
    int hold_ticks_ = 0;        ///< stopping, then this many ticks still
    u32 held_for_ = 0;          ///< the unit it stops for
    /// A sidestep point is reached, not passed near.
    static constexpr f32 SIDESTEP_TOLERANCE = 0.25f;

    // Moho pathing (roadmap item 4c-2c).
    path::PathNavigator moho_;
    bool moho_pending_ = false; ///< a goal set, taken up at the next update
    bool moho_active_ = false;  ///< following moho_
    /// Its target is outside the goal: driven through at speed, not stopped
    /// on (Moho's IAiSteering::UseTopSpeed).
    bool through_target_ = false;
    /// The target cell its waypoint was last set from.
    path::Cell moho_waypoint_cell_{-32768, -32768};
    Vector3 last_pos_{}; ///< where it was last update (Moho's last transform)
    // What a goal was set with, for the grid pathfinder should the unit
    // have no footprint class.
    std::string moho_layer_;
    f32 moho_draft_ = 0;
    bool moho_amphibious_ = false;

    // Memo of the last outright path failure (see FAILED_PATH_RETRY_CALLS).
    bool has_failed_request_ = false;
    Vector3 failed_goal_;
    Vector3 failed_from_;
    int suppressed_requests_ = 0;
    static constexpr f32 ARRIVAL_TOLERANCE = 0.5f;
    static constexpr f32 WAYPOINT_TOLERANCE = 1.5f;
};

} // namespace osc::sim
