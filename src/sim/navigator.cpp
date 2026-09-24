#include "sim/navigator.hpp"
#include "core/dmath.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "map/pathfinder.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace osc::sim {

void Navigator::set_goal(const Vector3& pos, const map::Pathfinder* pathfinder,
                          const Vector3& current_pos,
                          const std::string& layer,
                          f32 draft, bool amphibious) {
    goal_ = pos;
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
    goal_ = pos;
    waypoints_.clear();
    waypoint_index_ = 0;
    best_dist_ = 1e30f;
    stalled_ = 0;
    waypoints_.push_back(pos);
    status_ = Status::Moving;
}

void Navigator::abort_move() {
    status_ = Status::Idle;
    waypoints_.clear();
    waypoint_index_ = 0;
}

bool Navigator::update(Unit& unit, f32 max_speed, f64 dt, const map::Terrain* terrain) {
    if (unit.is_air_unit()) return slide(unit, max_speed, dt, terrain);
    return drive(unit, max_speed, dt, terrain);
}

void Navigator::arrive() {
    status_ = Status::Idle;
    waypoints_.clear();
    waypoint_index_ = 0;
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
    Vector3 pos = unit.position();
    f32 heading = quat_yaw(unit.orientation());
    f32 speed = unit.ground_speed();

    // Held still (a weapon unpacking, a teleport): it keeps its orders.
    if (unit.immobile()) {
        unit.note_drive(0, 0, max_speed, Unit::MotionTurn::Straight);
        return true;
    }

    // Waypoints along the way are passed within reach, or once crossed: past
    // the line through them square to the path on.
    while (waypoint_index_ + 1 < waypoints_.size()) {
        const Vector3& wp = waypoints_[waypoint_index_];
        const Vector3& next = waypoints_[waypoint_index_ + 1];
        const f32 dx = wp.x - pos.x;
        const f32 dz = wp.z - pos.z;
        const bool reached = dx * dx + dz * dz <= WAYPOINT_TOLERANCE * WAYPOINT_TOLERANCE;
        const bool crossed =
            (pos.x - wp.x) * (next.x - wp.x) + (pos.z - wp.z) * (next.z - wp.z) > 0;
        if (!reached && !crossed) break;
        ++waypoint_index_;
    }
    const bool final = waypoint_index_ + 1 == waypoints_.size();
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
    const bool crowded =
        final && stalled_ >= CROWD_TICKS && dist <= 2.0f + 3.0f * unit.separation_radius();

    // There: within reach, and slow enough to stop in a tick (or the goal
    // has fallen behind it).
    if (crowded || (final && dist <= ARRIVAL_TOLERANCE &&
                    (std::abs(speed) <= brake * step + 1e-3f || std::abs(err) > REVERSE_ANGLE ||
                     dist <= 0.05f))) {
        unit.note_drive(0, 0, max_speed, Unit::MotionTurn::Straight);
        arrive();
        return false;
    }
    const bool reverse = final && d.max_speed_reverse > 0 && std::abs(err) > REVERSE_ANGLE &&
                         to_goal <= d.backup_distance;
    if (reverse) err = wrap_angle(err + kPi);

    // Turn: at its TurnRate, faster at speed where its TurnRadius allows.
    f32 omega = d.turn_rate * unit.turn_mult();
    if (d.turn_radius > 0 && dist > d.turn_radius)
        omega = std::max(omega, std::abs(speed) / d.turn_radius);
    const f32 max_turn = omega * step;
    const f32 turn = std::clamp(err, -max_turn, max_turn);
    heading = wrap_angle(heading + turn);
    err -= turn;
    const Unit::MotionTurn turning = std::abs(turn) < 1e-3f ? Unit::MotionTurn::Straight
                                     : std::abs(turn) >= max_turn * 0.99f
                                         ? Unit::MotionTurn::SharpTurn
                                         : Unit::MotionTurn::Turn;

    // How fast it wants to go: slower the farther off its heading the
    // waypoint lies. One that can pivot stops to; one that can't keeps
    // moving to turn. Always slow enough to stop on its goal.
    const f32 top = reverse ? std::min(d.max_speed_reverse, max_speed) : max_speed;
    f32 target = top * std::max(osc::dmath::cos(err), 0.0f);
    if (d.rotate_on_spot && std::abs(err) > PIVOT_ANGLE) target = 0;
    else if (!d.rotate_on_spot) target = std::max(target, top * TURNING_SPEED);
    // No faster than lets it turn onto the waypoint: the circle along its
    // heading through it has radius dist / (2 sin err). Else it would circle
    // a goal inside its turn for ever.
    if (const f32 side = std::abs(osc::dmath::sin(err)); side > 1e-3f)
        target = std::min(target, omega * dist / (2.0f * side));
    // Braking to stop on its goal, a tick ahead so it doesn't overrun.
    const f32 stop_at =
        brake > 0 ? std::sqrt(2.0f * brake * std::max(to_goal - std::abs(speed) * step, 0.0f))
                  : target;
    const bool stopping = stop_at < target;
    target = std::min(target, stop_at);

    // Speed up at its acceleration, slow down at its brake.
    const f32 want = reverse ? -target : target;
    const bool faster = want > 0 ? speed >= 0 && want > speed : speed <= 0 && want < speed;
    const f32 rate = (faster ? d.max_accel * unit.accel_mult() : brake) * step;
    speed = speed < want ? std::min(want, speed + rate) : std::max(want, speed - rate);
    if (d.rotate_on_spot && target == 0 && std::abs(speed) <= d.rotate_threshold) speed = 0;

    // Drive along its heading.
    pos.x += osc::dmath::sin(heading) * speed * step;
    pos.z += osc::dmath::cos(heading) * speed * step;
    if (terrain) pos.y = terrain->get_surface_height(pos.x, pos.z);
    if (sim_) pos = sim_->clamp_to_playable(pos);
    unit.set_position(pos);
    unit.set_orientation(euler_to_quat(heading, 0.0f, 0.0f));
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

    auto pos = entity.position();
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
                if (sim_) pos = sim_->clamp_to_playable(pos);
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
                if (sim_) pos = sim_->clamp_to_playable(pos);
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
        if (sim_) pos = sim_->clamp_to_playable(pos);
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
    if (sim_) pos = sim_->clamp_to_playable(pos);
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

    f32 fdt = static_cast<f32>(dt);
    auto pos = unit.position();

    // Current waypoint target
    const auto& wp = waypoints_[waypoint_index_];
    bool is_final = (waypoint_index_ == waypoints_.size() - 1);

    // --- 1. Heading: turn toward target ---
    f32 dx = wp.x - pos.x;
    f32 dz = wp.z - pos.z;
    f32 desired_heading = osc::dmath::atan2(dx, dz); // atan2(x,z) for Y-up heading
    f32 heading = unit.heading();

    // Shortest-arc angle difference
    f32 angle_diff = desired_heading - heading;
    while (angle_diff > 3.14159265f) angle_diff -= 6.28318530f;
    while (angle_diff < -3.14159265f) angle_diff += 6.28318530f;

    f32 max_turn = unit.turn_rate_rad() * unit.turn_mult() * fdt;
    f32 actual_turn = 0;
    if (std::abs(angle_diff) <= max_turn) {
        heading = desired_heading;
        actual_turn = angle_diff;
    } else {
        f32 sign = (angle_diff > 0) ? 1.0f : -1.0f;
        heading += sign * max_turn;
        actual_turn = sign * max_turn;
    }
    while (heading < 0) heading += 6.28318530f;
    while (heading >= 6.28318530f) heading -= 6.28318530f;
    unit.set_heading(heading);

    // --- 2. Banking: proportional to turn rate ---
    f32 bank = std::clamp(actual_turn / fdt * 0.5f, -0.5f, 0.5f);
    f32 cur_bank = unit.bank_angle();
    cur_bank += (bank - cur_bank) * std::min(1.0f, 5.0f * fdt);
    unit.set_bank_angle(cur_bank);

    // --- 3. Acceleration ---
    f32 airspeed = unit.current_airspeed();
    f32 target_speed = unit.max_airspeed() * unit.speed_mult();
    f32 accel = unit.accel_rate() * unit.accel_mult();
    if (airspeed < target_speed) {
        airspeed = std::min(airspeed + accel * fdt, target_speed);
    } else if (airspeed > target_speed) {
        airspeed = std::max(airspeed - accel * fdt, target_speed);
    }
    unit.set_current_airspeed(airspeed);

    // --- 4. Move along heading ---
    f32 step = airspeed * fdt;
    pos.x += osc::dmath::sin(heading) * step;
    pos.z += osc::dmath::cos(heading) * step;

    // --- 5. Altitude management ---
    // Use get_terrain_height (NOT get_surface_height) — air units fly above terrain,
    // not above water surface. Ground navigator uses get_surface_height instead.
    f32 terrain_h = terrain ? terrain->get_terrain_height(pos.x, pos.z) : 0;
    f32 target_alt = unit.elevation_target();
    f32 alt = unit.current_altitude();
    f32 climb = unit.climb_rate() * fdt;
    if (alt < target_alt) {
        alt = std::min(alt + climb, target_alt);
    } else if (alt > target_alt) {
        alt = std::max(alt - climb, target_alt);
    }
    unit.set_current_altitude(alt);
    pos.y = terrain_h + alt;

    // --- 6. Pitch: visual dive/climb indication ---
    f32 target_y = terrain_h + target_alt;
    f32 pitch = (target_y - pos.y) * 0.02f;
    pitch = std::clamp(pitch, -0.3f, 0.3f);
    unit.set_pitch_angle(pitch);

    // --- 7. Set orientation from euler angles ---
    unit.set_orientation(euler_to_quat(heading, pitch, cur_bank));

    // --- 8. Clamp to playable area ---
    if (sim_) pos = sim_->clamp_to_playable(pos);
    unit.set_position(pos);

    // --- 9. Check waypoint arrival (2D distance) ---
    f32 dist2 = (wp.x - pos.x) * (wp.x - pos.x) + (wp.z - pos.z) * (wp.z - pos.z);
    f32 tolerance = is_final ? ARRIVAL_TOLERANCE : WAYPOINT_TOLERANCE;
    f32 air_tolerance = std::max(tolerance, airspeed * 0.5f);
    if (dist2 <= air_tolerance * air_tolerance) {
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

} // namespace osc::sim
