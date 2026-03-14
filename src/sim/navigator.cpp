#include "sim/navigator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "map/pathfinder.hpp"
#include "map/terrain.hpp"

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

    // Air units skip pathfinding — straight line
    if (layer == "Air" || !pathfinder) {
        waypoints_.push_back(pos);
        status_ = Status::Moving;
        return;
    }

    auto result = pathfinder->find_path(
        current_pos.x, current_pos.z, pos.x, pos.z, layer, draft, amphibious);

    if (result.found && !result.waypoints.empty()) {
        waypoints_ = std::move(result.waypoints);
        spdlog::debug("Navigator: path found with {} waypoints", waypoints_.size());
    } else {
        // Fallback: straight-line
        waypoints_.push_back(pos);
        spdlog::debug("Navigator: no path found, falling back to straight line");
    }

    status_ = Status::Moving;
}

void Navigator::set_goal(const Vector3& pos) {
    goal_ = pos;
    waypoints_.clear();
    waypoint_index_ = 0;
    waypoints_.push_back(pos);
    status_ = Status::Moving;
}

void Navigator::abort_move() {
    status_ = Status::Idle;
    waypoints_.clear();
    waypoint_index_ = 0;
}

bool Navigator::update(Entity& entity, f32 max_speed, f64 dt,
                        const map::Terrain* terrain) {
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
    f32 desired_heading = std::atan2(dx, dz); // atan2(x,z) for Y-up heading
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
    pos.x += std::sin(heading) * step;
    pos.z += std::cos(heading) * step;

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
