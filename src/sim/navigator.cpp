#include "sim/navigator.hpp"
#include "map/pathfinder.hpp"
#include "map/terrain.hpp"

#include <cmath>
#include <spdlog/spdlog.h>

namespace osc::sim {

void Navigator::set_goal(const Vector3& pos, const map::Pathfinder* pathfinder,
                          const Vector3& current_pos,
                          const std::string& layer) {
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
        current_pos.x, current_pos.z, pos.x, pos.z, layer);

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
    entity.set_position(pos);
    return true;
}

} // namespace osc::sim
