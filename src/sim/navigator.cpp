#include "sim/navigator.hpp"
#include "core/dmath.hpp"
#include "sim/air_combat.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/flight_math.hpp"
#include "sim/path_tables.hpp"
#include "sim/prepare_move.hpp"
#include "sim/sim_state.hpp"
#include "sim/steering.hpp"
#include "sim/unit.hpp"
#include "map/pathfinder.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <spdlog/spdlog.h>

namespace osc::sim {

void Navigator::set_goal(const Vector3& pos, const map::Pathfinder* pathfinder,
                          const Vector3& current_pos,
                          const std::string& layer,
                          f32 draft, bool amphibious) {
    ended_ = false;
    // Moho pathing: the goal is taken up by navigate() in the next army phase, where its
    // footprint is to hand (Moho's SetGoal only sets it thinking). Asked again
    // for the goal it already has, it carries on.
    if (sim_ && sim_->moho_pathing() && sim_->path_tables() && layer != "Air") {
        const bool same = moho_active_ && pos.x == goal_.x && pos.z == goal_.z;
        goal_ = pos;
        moho_pending_ = true;
        moho_layer_ = layer;
        moho_draft_ = draft;
        moho_amphibious_ = amphibious;
        if (!same) status_ = Status::WaitingForPath;
        return;
    }
    reset_moho();
    set_goal_grid(pos, pathfinder, current_pos, layer, draft, amphibious);
}

void Navigator::set_goal_grid(const Vector3& pos, const map::Pathfinder* pathfinder,
                              const Vector3& current_pos, const std::string& layer, f32 draft,
                              bool amphibious) {
    goal_ = pos;
    reset_steering();
    waypoints_.clear();
    waypoint_index_ = 0;
    best_dist_ = 1e30f;
    stalled_ = 0;

    // Air units skip pathfinding — straight line
    if (layer == "Air" || !pathfinder) {
        waypoints_.push_back(pos);
        status_ = Status::Moving;
        return;
    }

    // Same unreachable question as last time? Answer from the memo.
    auto near = [](const Vector3& a, const Vector3& b) {
        const f32 dx = a.x - b.x, dz = a.z - b.z;
        return dx * dx + dz * dz < 1.0f;
    };
    if (has_failed_request_ && near(pos, failed_goal_) &&
        near(current_pos, failed_from_) &&
        ++suppressed_requests_ < FAILED_PATH_RETRY_CALLS) {
        status_ = Status::Idle;
        return;
    }
    has_failed_request_ = false;
    suppressed_requests_ = 0;

    auto result = pathfinder->find_path(
        current_pos.x, current_pos.z, pos.x, pos.z, layer, draft, amphibious);

    if (result.found && !result.waypoints.empty()) {
        waypoints_ = std::move(result.waypoints);
        spdlog::trace("Navigator: path found with {} waypoints", waypoints_.size());
    } else if (result.throttled) {
        // Budget exhausted — don't fall back to straight-line (would clip walls).
        // Keep the goal and report "busy" so the command survives; its handler
        // re-requests the path next tick because is_moving() is false. (Going
        // Idle here made Move orders pop as if arrived and spun Patrol forever.)
        spdlog::trace("Navigator: pathfinding throttled, will retry next tick");
        status_ = Status::WaitingForPath;
        return;
    } else {
        // Nowhere reachable to go (enclosed, or no passable cell near the
        // goal). Stay put: the old straight-line fallback drove units
        // through cliffs and buildings.
        spdlog::trace("Navigator: no reachable destination, not moving");
        has_failed_request_ = true;
        failed_goal_ = pos;
        failed_from_ = current_pos;
        status_ = Status::Idle;
        return;
    }

    status_ = Status::Moving;
}

void Navigator::set_goal(const Vector3& pos) {
    ended_ = false;
    reset_moho();
    goal_ = pos;
    reset_steering();
    waypoints_.clear();
    waypoint_index_ = 0;
    best_dist_ = 1e30f;
    stalled_ = 0;
    waypoints_.push_back(pos);
    status_ = Status::Moving;
}

void Navigator::abort_move() {
    ended_ = false;
    air_hold_set_ = false;
    reset_moho();
    status_ = Status::Idle;
    waypoints_.clear();
    waypoint_index_ = 0;
    reset_steering();
}

void Navigator::reset_steering() {
    collision_ = {};
    next_check_ = 0;
    sidestep_ = false;
    sidestep_index_ = 0;
    hold_ticks_ = 0;
    held_for_ = 0;
    gear_fresh_ = true; // its next target chooses its gear again
}

void Navigator::path_ahead(const Vector3& from, f32 speed, f32 top_speed, f32 accel, int nodes,
                           std::vector<Vector3>& out) const {
    out.clear();
    if (status_ != Status::Moving || waypoint_index_ >= waypoints_.size() || nodes <= 0) return;
    // On Moho's spline, its nodes from the next are where it will be (CheckCollisions).
    if (moho_active_ && has_spline_ && !spline_.empty()) {
        for (size_t k = spline_.current();
             k < spline_.nodes().size() && static_cast<int>(out.size()) < nodes; ++k)
            out.push_back(spline_.nodes()[k].position);
        return;
    }
    constexpr f32 kTick = 0.1f;
    Vector3 p = from;
    f32 v = std::abs(speed);
    size_t i = waypoint_index_;
    out.push_back(p); // where it is: the path's current node
    while (static_cast<int>(out.size()) < nodes && i < waypoints_.size()) {
        v = accel > 0 ? std::min(top_speed, v + accel * kTick) : top_speed;
        f32 left = v * kTick;
        while (left > 0 && i < waypoints_.size()) {
            const f32 dx = waypoints_[i].x - p.x, dz = waypoints_[i].z - p.z;
            const f32 d = std::sqrt(dx * dx + dz * dz);
            if (d <= left) {
                p.x = waypoints_[i].x;
                p.z = waypoints_[i].z;
                left -= d;
                ++i;
            } else {
                p.x += dx / d * left;
                p.z += dz / d * left;
                left = 0;
            }
        }
        out.push_back(p);
    }
}

void Navigator::sidestep(const Vector3& point) {
    if (status_ != Status::Moving || waypoint_index_ > waypoints_.size()) return;
    waypoints_.insert(waypoints_.begin() + static_cast<std::ptrdiff_t>(waypoint_index_), point);
    sidestep_ = true;
    sidestep_index_ = waypoint_index_;
}

void Navigator::hold(int ticks, u32 for_id) {
    hold_ticks_ = std::max(ticks, 1);
    held_for_ = for_id;
}

bool Navigator::update(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain) {
    if (unit.is_air_unit()) return slide(unit, max_speed, dt, terrain);
    if (moho_pending_ || moho_active_) {
        return drive_moho(unit, max_speed, dt, terrain);
    }
    return drive(unit, max_speed, dt, terrain);
}

void Navigator::reset_moho() {
    spline_stop();
    steer_dest_set_ = false;
    waypoint_refresh_ = false;
    moho_.reset();
    moho_pending_ = false;
    moho_active_ = false;
    through_target_ = false;
    moho_waypoint_cell_ = {-32768, -32768};
}

namespace {

/// Moho's ELayer bits, the navigator's layer token (a ground unit's layers:
/// it never paths in orbit).
u32 layer_token(const std::string& layer) {
    if (layer == "Land") return 0x01;
    if (layer == "Seabed") return 0x02;
    if (layer == "Sub") return 0x04;
    if (layer == "Water") return 0x08;
    if (layer == "Air") return 0x10;
    return 0;
}

/// Moho's default pathcap by the map's larger side (CArmyImpl
/// ResolveDefaultPathCapacity).
i32 default_pathcap(u32 extent) {
    if (extent < 512) return 500;
    if (extent < 1024) return 1000;
    if (extent < 2048) return 2000;
    if (extent < 4096) return 10000;
    return 20000;
}

} // namespace

void Navigator::navigate(Unit& unit) {
    if (!moho_pending_ && !moho_active_) {
        return;
    }
    using State = path::PathNavigator::State;
    PathTables* tables = sim_ ? sim_->path_tables() : nullptr;
    ArmyBrain* army = sim_ ? sim_->get_army(unit.army()) : nullptr;
    const map::Terrain* map = sim_ ? sim_->terrain() : nullptr;
    const Vector3 pos = unit.position();
    if (moho_pending_) {
        moho_pending_ = false;
        const i32 cls = unit.footprint_class();
        if (!tables || !army || !map || cls < 0 || static_cast<size_t>(cls) >= tables->size()) {
            // No class to path as: the grid pathfinder, as before.
            reset_moho();
            set_goal_grid(goal_, sim_ ? sim_->pathfinder() : nullptr, pos, moho_layer_, moho_draft_,
                          moho_amphibious_);
            ended_ = !busy();
            return;
        }
        // Moho's move goal: the cell the footprint would stand on there, the
        // destination first moved to a place it fits (Unit::PrepareMove).
        const blueprints::Footprint& fp = unit.footprint();
        Vector3 dest = goal_;
        prepare_move(unit, *map, sim_->occupancy(), sim_->move_bounds(unit.army()), dest);
        const OccupancyRect at = footprint_rect(fp, dest.x, dest.z);
        const path::NavGoal goal{{at.x0, at.z0, at.x0 + 1, at.z0 + 1}, {}};
        // The move it is on, asked again (an order re-asserts its goal while
        // it waits for a path): it carries on, whatever its path navigator
        // says. One its army's queue failed is heard below and ends, as
        // Moho's move task ends on the navigator's Failed; taken
        // for a new goal, it would search again, round and round. A move
        // that ended has cleared moho_active_.
        const bool same =
            moho_active_ && moho_.goal().outer.x0 == at.x0 && moho_.goal().outer.z0 == at.z0;
        if (!same) {
            moho_.set_unit(fp, cls, unit.layer() == "Water");
            moho_.set_goal(goal, pos.x, pos.z);
            reset_steering();
            best_dist_ = 1e30f;
            stalled_ = 0;
            moho_waypoint_cell_ = {-32768, -32768};
            if (!moho_active_) last_pos_ = pos;
        }
        moho_active_ = true;
    }
    if (!tables || !army || !map) {
        reset_moho();
        arrive();
        ended_ = true;
        return;
    }
    // CAiNavigatorLand::Execute: the path navigator first.
    const bool moved = pos.x != last_pos_.x || pos.y != last_pos_.y || pos.z != last_pos_.z;
    last_pos_ = pos;
    path::NavUnit nav;
    nav.x = pos.x;
    nav.y = pos.y;
    nav.z = pos.z;
    nav.moved = moved;
    nav.immobile = unit.immobile();
    nav.attacking = unit.has_unit_state("Attacking");
    nav.layer = layer_token(unit.layer());
    nav.waiting_for_transport = !unit.command_queue().empty() &&
                                unit.command_queue().front().type == CommandType::TransportLoad;
    nav.tick = sim_->tick_count();
    nav.probe_bucket = EntityRegistry::moho_id(unit.entity_id()) % 7;
    nav.max_speed = unit.max_speed();
    // TurnRadius against TurnRate in its blueprint's units, degrees a second.
    constexpr f32 kRadToDeg = 180.0f / 3.14159265358979f;
    nav.turns_wide = unit.drive().turn_radius > unit.drive().turn_rate * kRadToDeg;
    nav.surfacing_sub = unit.motion_type() == "RULEUMT_SurfacingSub";
    const OccupancyRect playable = sim_->move_bounds(unit.army(), false);
    path::PathWorld world{map, &sim_->occupancy(), playable, army->use_whole_map(),
                          default_pathcap(std::max(map->map_width(), map->map_height()))};
    world.blockers = &sim_->unit_blockers();
    world.owner = unit.entity_id();
    world.personal_pos_max_distance = sim_->navigator_personal_pos_max_distance();
    moho_.update(nav, world, army->path_queue());
    const State st = moho_.state();
    if (st == State::Idle || st == State::Failed) {
        // There (or as near as it gets), or given up: CAiNavigatorLand::Execute stops its
        // steering first (Stop: off its spline, its meetings checked afresh if nodes were
        // left), then goes idle, handing the steering no waypoints, and nothing is left to
        // brake on. Undriven, it coasts.
        if (sim_->steer_log())
            *sim_->steer_log() << "STEER t=" << sim_->tick_count() << ' ' << unit.blueprint_id()
                               << " waypoints none #" << EntityRegistry::moho_id(unit.entity_id())
                               << '\n';
        steer_stop(*sim_, unit);
        reset_moho();
        arrive();
        ended_ = true;
        return;
    }
    // Then the steering, at the target: a new target replaces its waypoint
    // (and any sidestep); one outside the goal it drives through.
    const path::Cell target = moho_.target();
    if (target != moho_waypoint_cell_ || waypoints_.empty()) {
        if (target != moho_waypoint_cell_ && sim_->steer_log()) {
            *sim_->steer_log() << "STEER t=" << sim_->tick_count() << ' ' << unit.blueprint_id()
                               << " waypoints " << moho_.target_x() << ',' << moho_.target_z()
                               << " #" << EntityRegistry::moho_id(unit.entity_id()) << '\n';
        }
        moho_waypoint_cell_ = target;
        // CAiSteeringImpl::DriveToNextWaypoint: a waypoint the unit is at (Unit::IsAtPosition)
        // sets no motion target.
        if (target != moho_.current()) {
            waypoint_refresh_ = true;
            waypoints_.assign(1, Vector3{moho_.target_x(), pos.y, moho_.target_z()});
            waypoint_index_ = 0;
            sidestep_ = false;
            // UseTopSpeed(IsCellInGoal) picks the flag GetVal reads: in the goal, the
            // order's speed through it (CalcAtTopSpeed1); short of it, none left of its
            // path and not thinking (CalcAtTopSpeed2). CAiNavigatorLand::Execute, 0x005A4280.
            through_target_ = moho_.cell_in_goal(target) ? speed_through_goal_
                                                         : status_ != Status::WaitingForPath &&
                                                               moho_.last_node_index() < 0;
        } else {
            // At it, the steering still takes it for its destination (mDestination), what a
            // path asked afresh after a push makes for.
            set_steer_destination(Vector3{moho_.target_x(), pos.y, moho_.target_z()});
        }
    }
    status_ = st == State::HasPath ? Status::Moving : Status::WaitingForPath;
}

bool Navigator::drive_moho(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain) {
    if (!moho_active_ || waypoints_.empty()) {
        return true;
    }
    const Status was = status_;
    status_ = Status::Moving;
    drive_spline(unit, max_speed, dt, terrain);
    if (status_ != Status::Idle) {
        status_ = was;
    }
    return true;
}

void Navigator::arrive() {
    status_ = Status::Idle;
    waypoints_.clear();
    waypoint_index_ = 0;
    reset_steering();
}

namespace {

constexpr f32 kPi = 3.14159265358979f;

/// An angle brought into (-pi, pi].
f32 wrap_angle(f32 a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

} // namespace

namespace {

/// What Moho's spline reads of a unit.
path::SplineUnit spline_unit(const Unit& unit) {
    const Unit::Drive& d = unit.drive();
    path::SplineUnit su;
    su.max_speed = unit.max_speed();
    su.max_speed_reverse = d.max_speed_reverse;
    su.max_accel = d.max_accel > 0 ? d.max_accel : unit.max_speed();
    su.max_brake = d.max_brake;
    su.max_steer_force = d.max_steer_force;
    su.turn_rate = d.turn_rate * 57.29578f; // the blueprint's degrees
    su.turn_radius = d.turn_radius;
    su.rotate_on_spot = d.rotate_on_spot;
    su.rotate_threshold = d.rotate_threshold;
    su.backup_distance = d.backup_distance;
    su.size_x = unit.size_x();
    su.size_z = unit.size_z();
    su.speed_mult = unit.speed_mult();
    su.accel_mult = unit.accel_mult();
    su.turn_mult = unit.turn_mult();
    // Its nav speed carries its speed multiplier, which the spline applies itself.
    su.top_speed = su.speed_mult > 0 ? unit.nav_speed() / su.speed_mult : unit.nav_speed();
    return su;
}

/// Where it is: its position, heading and last tick's step.
path::Pose spline_pose(const Unit& unit) {
    // Its forward axis straight from its orientation, pitch and all, as Moho's
    // QuatForwardAxis (Generate, 0x005B30E3): a unit made side-on to its goal has a
    // forward z of float noise, whose sign picks forward or reverse.
    const Quaternion& q = unit.orientation();
    const Vector3 forward{(q.w * q.y + q.z * q.x) * 2.0f, (q.z * q.y - q.w * q.x) * 2.0f,
                          1.0f - (q.y * q.y + q.x * q.x) * 2.0f};
    // Its velocity a tick, as Unit::GetVelocity (0x006A9C90) gives it: off the ground, its
    // motion's own (CUnitMotion's mVelocity), not the step between its last two places.
    if (unit.layer() != "Air") return {unit.position(), forward, unit.motion_velocity()};
    const Vector3& v = unit.velocity();
    constexpr f32 kTick = 0.1f;
    return {unit.position(), forward, {v.x * kTick, v.y * kTick, v.z * kTick}};
}

} // namespace

bool Navigator::unit_at(const Unit& unit, const Vector3& p) {
    const f32 hx = unit.footprint().size_x * 0.5f;
    const f32 hz = unit.footprint().size_z * 0.5f;
    const Vector3& at = unit.position();
    return std::lrint(at.x - hx) == std::lrint(p.x - hx) &&
           std::lrint(at.z - hz) == std::lrint(p.z - hz);
}

namespace {

/// A node's height as Generate and Update give it: the terrain's, or the water's where a
/// ship, a floating amphibian or a hover rides above it.
void set_node_height(path::SplineWorld& world, const map::Terrain* terrain, const Unit& unit) {
    if (!terrain) return;
    const std::string& motion = unit.motion_type();
    const bool rides_water = motion == "RULEUMT_Water" || motion == "RULEUMT_AmphibiousFloating" ||
                             motion == "RULEUMT_Hover";
    world.height = [terrain, rides_water](f32 x, f32 z) {
        const f32 ground = terrain->get_terrain_height(x, z);
        return rides_water && terrain->has_water() && terrain->water_elevation() > ground
                   ? terrain->water_elevation()
                   : ground;
    };
}

} // namespace

void Navigator::spline_generate(const Unit& unit, const Vector3& to, path::PathType type,
                                bool carry_on) {
    collision_ = {}; // UpdatePath starts its collision afresh
    path::SplineWorld world;
    if (sim_) {
        set_node_height(world, sim_->terrain(), unit);
        world.wont_fit = [&](f32 x, f32 z) {
            return sim_->footprint_fits_at(unit.footprint(), x, z) == 0;
        };
        // A unit that turns wider than it turns fast brakes for one standing where it
        // would go (Generate's PPS_7: UnitIsBlockedAt, mode 2).
        world.blocked = [&](f32 x, f32 z) {
            return sim_->unit_blockers().unit_blocked_at(unit.entity_id(), x, z, 2);
        };
    }
    spline_.generate(spline_unit(unit), spline_pose(unit), to, type, carry_on, unit_at(unit, to),
                     world);
    has_spline_ = true;
}

void Navigator::spline_brake(const Unit& unit, int mode) {
    collision_ = {};
    path::SplineWorld world;
    if (sim_) set_node_height(world, sim_->terrain(), unit);
    spline_.brake(spline_unit(unit), spline_pose(unit), mode, world);
    has_spline_ = true;
}

void Navigator::spline_stop() {
    spline_.clear();
    has_spline_ = false;
    motion_node_set_ = false;
    avoid_target_ = {};
    collision_ = {};
}

void Navigator::advance_motion_node() {
    if (!has_spline_) return; // the motion keeps what it holds
    const path::Node* node = spline_.advance();
    motion_node_set_ = node != nullptr;
    if (node) motion_node_ = *node;
}

void Navigator::drive_spline(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain) {
    if (unit.immobile()) {
        unit.note_drive(0, 0, max_speed, Unit::MotionTurn::Straight);
        return;
    }
    // No node handed over, or pushed: undriven, it coasts on its velocity (CalcMoveCommon's
    // branch for no node or mIsBeingPushed; Unit::coast_moho).
    // The node stays the motion's till its steering hands over the next (mNextWaypoint).
    if (!motion_node_set_ || unit.pushed()) return;
    const auto step = static_cast<f32>(dt);
    const Unit::Drive& d = unit.drive();
    Vector3 pos = unit.pending_position();
    const Vector3 from = pos;
    const f32 heading_from = quat_yaw(unit.pending_orientation());
    const path::Node* node = &motion_node_;

    pos.x = node->position.x;
    pos.z = node->position.z;
    const f32 heading = osc::dmath::atan2(node->direction.x, node->direction.z);
    // Facing the node's heading as CalcMoveCommon sets it (COORDS_Orient), tilted to the
    // ground unnormalized: the motion normalizes once, in MoveTo.
    const Quaternion level = coords_orient_moho(node->direction);
    Quaternion facing = level;
    if (terrain) {
        const Unit::GroundStance stance = unit.ground_stance(terrain, pos.x, pos.z, level, false);
        pos.y = stance.y;
        facing = stance.orientation;
    }
    if (sim_) pos = sim_->clamp_to_playable(pos, unit.army());
    const f32 turn = std::abs(wrap_angle(heading - heading_from));
    f32 max_turn = d.turn_rate * unit.turn_mult() * step;
    if (d.turn_radius > 0)
        max_turn = std::max(max_turn, std::abs(unit.ground_speed()) * step * unit.turn_mult() /
                                          d.turn_radius);
    const Unit::MotionTurn turning = turn < 1e-3f               ? Unit::MotionTurn::Straight
                                     : turn >= max_turn * 0.99f ? Unit::MotionTurn::SharpTurn
                                                                : Unit::MotionTurn::Turn;
    // A step onto ground it won't fit leaves it where it was (CalcMoveCommon), and
    // its steering asks for the way afresh.
    if (moho_active_ && sim_ && sim_->footprint_fits_at(unit.footprint(), pos.x, pos.z) == 0 &&
        sim_->footprint_fits_at(unit.footprint(), from.x, from.z) != 0) {
        // It stays, its velocity gone, and is pushed back toward where it was
        // (CalcMoveCommon: AddImpulse at a hundredth of its top speed).
        unit.set_motion_velocity({});
        unit.set_motion_pos(from);
        const f32 bx = from.x - pos.x;
        const f32 bz = from.z - pos.z;
        const f32 blen = std::sqrt(bx * bx + bz * bz);
        if (!unit.pushed() && blen > 0) {
            const f32 scale = unit.nav_speed() * 0.010000001f / blen;
            unit.add_impulse({bx * scale, 0, bz * scale}, true);
        }
        unit.set_orientation(
            terrain ? unit.ground_stance(terrain, from.x, from.z, level, false).orientation
                    : level);
        unit.note_drive(0, 0, max_speed, turning);
        return;
    }
    unit.set_position(pos);
    unit.set_orientation(facing);
    // Its step from mPos to the node, the node's own height and all (CalcMoveCommon).
    const Vector3& was = unit.motion_pos();
    unit.set_motion_velocity(
        {node->position.x - was.x, node->position.y - was.y, node->position.z - was.z});
    unit.set_motion_pos(node->position);
    // Its speed along its heading; it is stopping within a second's run of its target
    // (CUnitMotion::ProcessCommonMotionState).
    const f32 speed = ((pos.x - from.x) * osc::dmath::sin(heading) +
                       (pos.z - from.z) * osc::dmath::cos(heading)) /
                      step;
    const Vector3* dest = steer_destination();
    const f32 tx = dest ? dest->x - pos.x : 0.0f;
    const f32 tz = dest ? dest->z - pos.z : 0.0f;
    const f32 second = unit.effective_speed();
    const bool near_target = !through_target_ && second * second > tx * tx + tz * tz;
    unit.note_drive(speed, near_target ? 0.0f : max_speed, max_speed, turning);
}

bool Navigator::drive(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain) {
    if (status_ == Status::WaitingForPath) return true; // not there yet
    if (status_ == Status::Idle || max_speed <= 0) return false;
    if (waypoints_.empty() || waypoint_index_ >= waypoints_.size()) {
        arrive();
        return false;
    }
    const auto step = static_cast<f32>(dt);
    // A unit made without its blueprint's physics reaches its top speed in a
    // second and turns 90 degrees in one.
    Unit::Drive d = unit.drive();
    if (d.max_accel <= 0) d.max_accel = max_speed;
    if (d.turn_rate <= 0) d.turn_rate = kPi * 0.5f;
    Vector3 pos = unit.pending_position();
    const Vector3 from = pos;
    f32 heading = quat_yaw(unit.pending_orientation());
    f32 speed = unit.ground_speed();
    // Moho's CUnitMotion refuses a move onto ground the unit won't fit (it
    // did where it was).
    const auto refused = [&](const Vector3& to) {
        return moho_active_ && sim_ && sim_->footprint_fits_at(unit.footprint(), to.x, to.z) == 0 &&
               sim_->footprint_fits_at(unit.footprint(), from.x, from.z) != 0;
    };

    // Held still (a weapon unpacking, a teleport): it keeps its orders.
    if (unit.immobile()) {
        unit.note_drive(0, 0, max_speed, Unit::MotionTurn::Straight);
        return true;
    }

    // Stopping for a unit crossing its way (M203c): it brakes at twice its
    // brake along its heading, then waits, still, before going on.
    if (hold_ticks_ > 0) {
        const f32 hard = 2.0f * (d.max_brake > 0 ? d.max_brake : d.max_accel) * unit.accel_mult();
        speed =
            speed > 0 ? std::max(0.0f, speed - hard * step) : std::min(0.0f, speed + hard * step);
        if (speed == 0 && --hold_ticks_ == 0) next_check_ = 0; // looks again as it goes on
        pos.x += osc::dmath::sin(heading) * speed * step;
        pos.z += osc::dmath::cos(heading) * speed * step;
        Quaternion facing = unit.pending_orientation();
        if (terrain) {
            const Unit::GroundStance stance = unit.ground_stance(terrain, pos.x, pos.z, facing);
            pos.y = stance.y;
            facing = stance.orientation;
        }
        if (sim_) pos = sim_->clamp_to_playable(pos, unit.army());
        // Braking near still, it stays put if refused, and isn't sent for a
        // fresh way: that strands more of a crowd's waiting units.
        if (refused(pos)) {
            unit.note_drive(0, 0, max_speed, Unit::MotionTurn::Straight);
            return true;
        }
        unit.set_position(pos);
        unit.set_orientation(facing);
        unit.note_drive(speed, 0, max_speed, Unit::MotionTurn::Straight);
        return true;
    }

    // Waypoints along the way are passed within reach, or once crossed: past
    // the line through them square to the path on.
    while (waypoint_index_ + 1 < waypoints_.size()) {
        const Vector3& wp = waypoints_[waypoint_index_];
        const Vector3& next = waypoints_[waypoint_index_ + 1];
        const f32 dx = wp.x - pos.x;
        const f32 dz = wp.z - pos.z;
        // A sidestep point (M203c) is only a little off the path: it is
        // reached, not passed near.
        const f32 tol = sidestep_ && waypoint_index_ == sidestep_index_ ? SIDESTEP_TOLERANCE
                                                                        : WAYPOINT_TOLERANCE;
        const bool reached = dx * dx + dz * dz <= tol * tol;
        const bool crossed =
            (pos.x - wp.x) * (next.x - wp.x) + (pos.z - wp.z) * (next.z - wp.z) > 0;
        if (!reached && !crossed) break;
        ++waypoint_index_;
    }
    // Past its sidestep point, it looks again as it goes on (M203c).
    if (sidestep_ && waypoint_index_ > sidestep_index_) {
        sidestep_ = false;
        next_check_ = 0;
    }
    // Following a Moho path, a target outside the goal is driven through, and
    // its navigator, not the drive, says when it is there.
    const bool through = moho_active_ && through_target_;
    const bool final = waypoint_index_ + 1 == waypoints_.size() && !through;
    const Vector3& wp = waypoints_[waypoint_index_];
    const f32 dx = wp.x - pos.x;
    const f32 dz = wp.z - pos.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    f32 to_goal = dist; // along the path
    for (size_t i = waypoint_index_ + 1; i < waypoints_.size(); ++i) {
        const f32 sx = waypoints_[i].x - waypoints_[i - 1].x;
        const f32 sz = waypoints_[i].z - waypoints_[i - 1].z;
        to_goal += std::sqrt(sx * sx + sz * sz);
    }
    // Forward, or backing up to a goal close behind.
    f32 err = dist > 1e-4f ? wrap_angle(osc::dmath::atan2(dx, dz) - heading) : 0.0f;
    // Dead behind it turns the positive way, as Moho's RotateDirectionTowardTargetLimited
    // (0x006992C0) does on a zero cross product.
    if (std::abs(err) > kPi - 1e-4f) err = kPi;
    // Moho caps the speed from the heading the unit has before this tick's turn.
    const f32 err_before_turn = err;
    const f32 brake = (d.max_brake > 0 ? d.max_brake : d.max_accel) * unit.accel_mult();

    // In a crowd others may hold its spot: jostled near it without getting
    // nearer, it is as close as it will get.
    if (final) {
        if (dist < best_dist_ - 0.1f) {
            best_dist_ = dist;
            stalled_ = 0;
        } else if (unit.jostled()) {
            ++stalled_;
        }
    }
    const bool crowded = !moho_active_ && final && stalled_ >= CROWD_TICKS &&
                         dist <= 2.0f + 3.0f * unit.separation_radius();

    // There: within reach, and slow enough to stop in a tick (or the goal
    // has fallen behind it).
    if (crowded || (final && dist <= ARRIVAL_TOLERANCE &&
                    (std::abs(speed) <= brake * step + 1e-3f || std::abs(err) > REVERSE_ANGLE ||
                     dist <= 0.05f))) {
        if (moho_active_) {
            unit.note_drive(0, 0, max_speed, Unit::MotionTurn::Straight);
            return true; // holds there till its navigator says
        }
        // Undriven, it coasts what speed Moho's stopping speed left it (Unit::coast).
        arrive();
        return false;
    }
    // Moho's spline states (faf-re CAiPathSpline::Generate): chosen for each new
    // target, then stepped below. A unit that backs up and doesn't rotate on the
    // spot, stopped (or slow, or turning wide) with its target behind, backs
    // while its nose comes round, or, the target within its BackUpDistance and
    // within 60 degrees of dead behind, backs straight to it tail first; going
    // on fast, it brakes first. Any other drives on.
    constexpr f32 kStopped = 0.01f;      // kStoppedSpeedRatio
    constexpr f32 kRadToDeg = 57.29578f; // Moho compares TurnRadius with TurnRate in degrees
    const f32 align = osc::dmath::cos(err_before_turn);
    const f32 ratio = max_speed > 0 ? std::abs(speed) / max_speed : 0.0f;
    const bool wide = d.turn_radius > d.turn_rate * kRadToDeg;
    if (gear_fresh_ || wp.x != gear_target_.x || wp.z != gear_target_.z) {
        gear_target_ = wp;
        gear_fresh_ = false;
        const bool backward = speed < 0;
        if (d.max_speed_reverse <= 0 || d.rotate_on_spot) gear_ = Gear::Forward;
        else if (align < 0 && (ratio < 0.5f || wide))
            gear_ = ratio > kStopped && !backward ? Gear::BrakeForward : Gear::Reverse;
        else if (backward) gear_ = align >= 0 ? Gear::BrakeReverse : Gear::Reverse;
        else gear_ = Gear::Forward;
        backup_ = gear_ == Gear::Reverse && d.backup_distance > dist && align < -0.5f;
    }
    const bool reverse = gear_ == Gear::Reverse;
    const bool coasting =
        gear_ == Gear::Stop || gear_ == Gear::BrakeForward || gear_ == Gear::BrakeReverse;
    if (backup_) err = wrap_angle(err + kPi); // its tail to the target, braking too (PPS_6)
    const f32 cap_err = err_before_turn;

    // Turn: at its TurnRate, faster at speed where its TurnRadius allows.
    f32 omega = d.turn_rate * unit.turn_mult();
    if (d.turn_radius > 0 && dist > d.turn_radius)
        omega = std::max(omega, std::abs(speed) / d.turn_radius);
    const f32 max_turn = omega * step;
    const f32 turn = std::clamp(err, -max_turn, max_turn);
    heading = wrap_angle(heading + turn);
    const Unit::MotionTurn turning = std::abs(turn) < 1e-3f ? Unit::MotionTurn::Straight
                                     : std::abs(turn) >= max_turn * 0.99f
                                         ? Unit::MotionTurn::SharpTurn
                                         : Unit::MotionTurn::Turn;

    // How fast it may go (Moho's steering speed cap, faf-re CAiPathSpline.cpp
    // ComputeSteeringSpeedCapFromParams, 0x00699760, from the heading before
    // this tick's turn):
    // - one that rotates on the spot, slower than its RotateOnSpotThreshold
    //   share of its top speed, stands until the waypoint is within acos 0.98
    //   (11.5 degrees) of its heading;
    // - any other goes as fast as lets it turn onto the waypoint along the
    //   circle through it tangent to its heading (radius dist / 2 sin err), at
    //   half its TurnRate; on a circle at least its TurnRadius wide, at that
    //   radius a tick. Always slow enough to stop on its goal (below).
    const f32 top = reverse ? std::min(d.max_speed_reverse, max_speed) : max_speed;
    f32 target = top;
    if (d.rotate_on_spot && d.rotate_threshold > ratio) {
        target = osc::dmath::cos(cap_err) >= 0.98f ? top : 0.0f;
    } else {
        const f32 side = std::abs(osc::dmath::sin(cap_err));
        const f32 circle = side > 0 ? dist / (2.0f * side) : 0.0f;
        const f32 radius = d.turn_radius > 0 ? d.turn_radius / unit.turn_mult()
                                             : std::numeric_limits<f32>::infinity();
        if (circle < radius) {
            if (circle > 0) target = d.turn_rate * unit.turn_mult() * circle * 0.5f;
        } else {
            target = radius / step; // the turn radius a tick
        }
    }
    target = std::min(target, top);
    // Braking to stop on its goal, at Moho's stopping speed (CAiPathSpline::Generate, PT_0):
    // sqrt(2 d brake) from where it is, or the whole way in a tick once a tick's brake covers it.
    const f32 stop_at =
        brake > 0 && !through
            ? (brake * step * step < to_goal ? std::sqrt(2.0f * brake * to_goal) : to_goal / step)
            : target;
    const bool stopping = stop_at < target;
    target = std::min(target, stop_at);
    // One that turns wider than it turns fast goes slower the further off it
    // faces (backing tail first, from its tail).
    if (wide) target *= (std::max(backup_ ? -align : align, -0.5f) + 1.0f) * 0.5f;
    if (coasting) target = 0; // braking to a stop: PPS_3, PPS_4, PPS_6

    // Speed up at its acceleration, slow down at its brake.
    const f32 want = reverse ? -target : target;
    const bool faster = want > 0 ? speed >= 0 && want > speed : speed <= 0 && want < speed;
    const f32 rate = (faster ? d.max_accel * unit.accel_mult() : brake) * step;
    speed = speed < want ? std::min(want, speed + rate) : std::max(want, speed - rate);
    if (d.rotate_on_spot && target == 0 && std::abs(speed) <= d.rotate_threshold) speed = 0;

    // Drive along its heading.
    pos.x += osc::dmath::sin(heading) * speed * step;
    pos.z += osc::dmath::cos(heading) * speed * step;
    const Quaternion level = euler_to_quat(heading, 0.0f, 0.0f);
    Quaternion facing = level;
    if (terrain) {
        const Unit::GroundStance stance = unit.ground_stance(terrain, pos.x, pos.z, level);
        pos.y = stance.y;
        facing = stance.orientation;
    }
    if (sim_) pos = sim_->clamp_to_playable(pos, unit.army());
    // The spline's states step: braking forward it stops, then backs (PPS_4);
    // backing, it goes on till its nose has come round to its target (tail
    // first: till it is within its braking distance) or the ground behind it
    // won't take it (PPS_5); braking back it stops, then drives on, or, tail
    // first, chooses again (PPS_6). Driving on, it brakes once its braking
    // distance is past its goal (PPS_7 to PPS_3), and, stopped, drives on
    // (its next spline's PPS_7).
    {
        const bool stopped = max_speed > 0 && std::abs(speed) / max_speed <= kStopped;
        const f32 decel = std::max(d.max_accel, d.max_brake);
        const f32 braking = decel > 0 ? speed * speed / (2.0f * decel) : 0.0f;
        const f32 rx = wp.x - pos.x, rz = wp.z - pos.z;
        const f32 left = std::sqrt(rx * rx + rz * rz);
        if (gear_ == Gear::Forward) {
            if (!through && braking > left + (to_goal - dist)) gear_ = Gear::Stop;
        } else if (gear_ == Gear::Stop && stopped) {
            gear_ = Gear::Forward;
        } else if (gear_ == Gear::BrakeForward && stopped) {
            gear_ = Gear::Reverse;
        } else if (gear_ == Gear::Reverse) {
            const f32 reach = std::max(unit.size_x(), unit.size_z()) + braking;
            const bool blocked =
                sim_ &&
                sim_->footprint_fits_at(unit.footprint(), pos.x - osc::dmath::sin(heading) * reach,
                                        pos.z - osc::dmath::cos(heading) * reach) == 0;
            const bool keep =
                backup_ ? braking <= left
                        : osc::dmath::cos(wrap_angle(osc::dmath::atan2(rx, rz) - heading)) <= 0.15f;
            if (blocked || !keep) gear_ = Gear::BrakeReverse;
        } else if (gear_ == Gear::BrakeReverse && stopped) {
            if (backup_) gear_fresh_ = true;
            gear_ = Gear::Forward;
            backup_ = false;
        }
    }
    // shortcut: Moho's PPS_7 brakes to back up when ground ahead won't take a sharp turn
    // (UnitWontFitAt, UnitIsBlockedAt); left out until a probe shows it.
    // Refused (as above): it stays where it was, as CalcMoveCommon puts it
    // back (its push, AddImpulse, is to its velocity, which its next node
    // replaces), and its steering asks for the way afresh (ProcessSplineMovement:
    // NotifyNavigatorPathRefresh). Wedged, it is still, so its navigator gives
    // the move up 30 ticks on: retail's Striker at a wall's end did. A sidestep
    // (M203c) that led there is given up.
    if (refused(pos)) {
        if (sidestep_ && waypoint_index_ == sidestep_index_ &&
            waypoint_index_ + 1 < waypoints_.size()) {
            waypoints_.erase(waypoints_.begin() + static_cast<std::ptrdiff_t>(waypoint_index_));
            sidestep_ = false;
            next_check_ = 0;
        }
        moho_.request_repath();
        unit.set_orientation(
            terrain ? unit.ground_stance(terrain, from.x, from.z, level).orientation : level);
        unit.note_drive(0, 0, top, turning);
        return true;
    }
    unit.set_position(pos);
    unit.set_orientation(facing);
    unit.note_drive(speed, stopping ? 0.0f : target, top, turning);
    return true;
}

bool Navigator::slide(Entity& entity, f32 max_speed, f64 dt, const map::Terrain* terrain) {
    if (status_ == Status::WaitingForPath) return true; // not there yet
    if (status_ == Status::Idle || max_speed <= 0) return false;
    if (waypoints_.empty() || waypoint_index_ >= waypoints_.size()) {
        status_ = Status::Idle;
        return false;
    }

    auto pos = entity.pending_position();
    f32 step = max_speed * static_cast<f32>(dt);

    // Process waypoints — may advance through multiple in one tick if fast enough
    while (waypoint_index_ < waypoints_.size()) {
        bool is_final = (waypoint_index_ == waypoints_.size() - 1);
        const auto& wp = waypoints_[waypoint_index_];
        f32 tolerance = is_final ? ARRIVAL_TOLERANCE : WAYPOINT_TOLERANCE;

        f32 dx = wp.x - pos.x;
        f32 dz = wp.z - pos.z;
        f32 dist2 = dx * dx + dz * dz;

        if (dist2 <= tolerance * tolerance) {
            // Reached this waypoint
            if (is_final) {
                pos.x = wp.x;
                pos.z = wp.z;
                if (terrain) pos.y = terrain->get_surface_height(pos.x, pos.z);
                if (sim_) pos = sim_->clamp_to_playable(pos, entity.army());
                entity.set_position(pos);
                status_ = Status::Idle;
                waypoints_.clear();
                waypoint_index_ = 0;
                return false;
            }
            // Advance to next waypoint
            waypoint_index_++;
            continue;
        }

        f32 dist = std::sqrt(dist2);

        if (step >= dist) {
            // Would overshoot — snap to waypoint
            pos.x = wp.x;
            pos.z = wp.z;
            step -= dist;
            if (is_final) {
                if (terrain) pos.y = terrain->get_surface_height(pos.x, pos.z);
                if (sim_) pos = sim_->clamp_to_playable(pos, entity.army());
                entity.set_position(pos);
                status_ = Status::Idle;
                waypoints_.clear();
                waypoint_index_ = 0;
                return false;
            }
            waypoint_index_++;
            continue;
        }

        // Move toward current waypoint
        f32 inv_dist = 1.0f / dist;
        pos.x += dx * inv_dist * step;
        pos.z += dz * inv_dist * step;
        break;
    }

    // If we exited the loop without breaking (exhausted all waypoints),
    // treat as arrived to avoid one-tick stale is_moving
    if (waypoint_index_ >= waypoints_.size()) {
        if (terrain) pos.y = terrain->get_surface_height(pos.x, pos.z);
        if (sim_) pos = sim_->clamp_to_playable(pos, entity.army());
        entity.set_position(pos);
        status_ = Status::Idle;
        waypoints_.clear();
        waypoint_index_ = 0;
        return false;
    }

    // Apply terrain height
    if (terrain) {
        pos.y = terrain->get_surface_height(pos.x, pos.z);
    }
    if (sim_) pos = sim_->clamp_to_playable(pos, entity.army());
    entity.set_position(pos);
    return true;
}

bool Navigator::update_air(Unit& unit, f64 dt,
                            const map::Terrain* terrain) {
    if (status_ == Status::WaitingForPath) return true; // air never throttles; defensive
    if (status_ == Status::Idle) return false;
    if (waypoints_.empty() || waypoint_index_ >= waypoints_.size()) {
        status_ = Status::Idle;
        return false;
    }

    Vector3 wp = waypoints_[waypoint_index_];
    const bool is_final = (waypoint_index_ == waypoints_.size() - 1);
    // CAiNavigatorAir::BuildGoalWorldPos: the centre of the goal's cell.
    const auto& fp = unit.footprint();
    if (fp.size_x > 0 && fp.size_z > 0) {
        const f32 hx = static_cast<f32>(fp.size_x) * 0.5f;
        const f32 hz = static_cast<f32>(fp.size_z) * 0.5f;
        wp.x = static_cast<f32>(std::lrint(wp.x - hx)) + hx;
        wp.z = static_cast<f32>(std::lrint(wp.z - hz)) + hz;
    }
    AirMove move;
    move.target = wp;
    move.elevation = unit.elevation_target();
    move.top_speed = speed_through_goal_ || !is_final;
    if (!air_hold_set_ || air_hold_.x != wp.x || air_hold_.z != wp.z) {
        const auto at = unit.pending_position();
        const f32 fx = wp.x - at.x;
        const f32 fz = wp.z - at.z;
        const f32 far = std::sqrt(fx * fx + fz * fz);
        if (far > unit.start_turn_distance()) {
            unit.set_air_facing({fx / far, 0.0f, fz / far});
        }
    }
    air_hold_ = wp;
    air_hold_set_ = true;
    fly_air_move(unit, move, sim_, terrain, static_cast<f32>(dt));

    // CUnitMotion::AtTarget, as CAiNavigatorAir::Execute asks it.
    const auto pos = unit.pending_position();
    const f32 dist2 = (wp.x - pos.x) * (wp.x - pos.x) + (wp.z - pos.z) * (wp.z - pos.z);
    f32 tolerance = 0.25f;
    if (!is_final) {
        tolerance = WAYPOINT_TOLERANCE;
    } else if (speed_through_goal_) {
        const AirCombatRules& r = unit.air_combat_rules();
        tolerance = std::max(tolerance,
                             unit.max_airspeed() * unit.speed_mult() * (r.winged ? 1.0f : 0.25f));
    }
    if (dist2 <= tolerance * tolerance) {
        if (is_final) {
            if (!speed_through_goal_) {
                status_ = Status::Idle;
                waypoints_.clear();
                waypoint_index_ = 0;
                return false;
            }
            status_ = Status::Idle;
            return false;
        }
        waypoint_index_++;
    }

    return true;
}

void Navigator::fingerprint_gear(Fnv& f) const {
    f.mix(static_cast<u64>(gear_) | static_cast<u64>(backup_ ? 1 : 0) << 8 |
          static_cast<u64>(gear_fresh_ ? 1 : 0) << 9);
    f.mix_f32(gear_target_.x);
    f.mix_f32(gear_target_.z);
}

void Navigator::fingerprint_moho(Fnv& f) const {
    f.mix(static_cast<u64>(moho_pending_ ? 1 : 0) | static_cast<u64>(moho_active_ ? 1 : 0) << 1 |
          static_cast<u64>(through_target_ ? 1 : 0) << 2 |
          static_cast<u64>(moho_amphibious_ ? 1 : 0) << 3);
    f.mix(path::pack_cell(moho_waypoint_cell_));
    // Where it was last update decides whether it has moved since.
    f.mix_f32(last_pos_.x);
    f.mix_f32(last_pos_.y);
    f.mix_f32(last_pos_.z);
    f.mix(moho_layer_.size());
    for (const char c : moho_layer_) f.mix(static_cast<u8>(c));
    f.mix_f32(moho_draft_);
    moho_.fingerprint(f);
    // Its spline: the node it is on, where its next is, and its steering's state.
    f.mix(spline_.nodes().size() << 8 | spline_.current() << 3 | (has_spline_ ? 1 : 0) |
          (waypoint_refresh_ ? 2 : 0) | (motion_node_set_ ? 4 : 0));
    if (!spline_.empty()) {
        const Vector3& at = spline_.nodes()[spline_.current()].position;
        f.mix_f32(at.x);
        f.mix_f32(at.z);
    }
    f.mix(collision_.type);
    f.mix_f32(steer_dest_.x);
    f.mix_f32(steer_dest_.z);
}

} // namespace osc::sim
