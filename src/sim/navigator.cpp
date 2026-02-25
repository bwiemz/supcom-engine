#include "sim/navigator.hpp"

#include <cmath>

namespace osc::sim {

void Navigator::set_goal(const Vector3& pos) {
    goal_ = pos;
    status_ = Status::Moving;
}

void Navigator::abort_move() {
    status_ = Status::Idle;
}

bool Navigator::update(Entity& entity, f32 max_speed, f64 dt) {
    if (status_ == Status::Idle || max_speed <= 0) return false;

    auto pos = entity.position();
    f32 dx = goal_.x - pos.x;
    f32 dz = goal_.z - pos.z;
    f32 dist2 = dx * dx + dz * dz;

    if (dist2 <= ARRIVAL_TOLERANCE * ARRIVAL_TOLERANCE) {
        // Snap to goal and stop
        entity.set_position({goal_.x, pos.y, goal_.z});
        status_ = Status::Idle;
        return false;
    }

    f32 dist = std::sqrt(dist2);
    f32 step = max_speed * static_cast<f32>(dt);

    if (step >= dist) {
        // Would overshoot — snap to goal
        entity.set_position({goal_.x, pos.y, goal_.z});
        status_ = Status::Idle;
        return false;
    }

    // Normalize direction and move
    f32 inv_dist = 1.0f / dist;
    pos.x += dx * inv_dist * step;
    pos.z += dz * inv_dist * step;
    entity.set_position(pos);
    return true;
}

} // namespace osc::sim
