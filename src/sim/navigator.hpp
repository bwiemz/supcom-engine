#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

namespace osc::sim {

class Navigator {
public:
    enum class Status : u8 { Idle, Moving };

    void set_goal(const Vector3& pos);
    void abort_move();

    const Vector3& goal() const { return goal_; }
    Status status() const { return status_; }
    bool is_moving() const { return status_ == Status::Moving; }

    /// Move entity toward goal. Returns true if still moving.
    bool update(Entity& entity, f32 max_speed, f64 dt);

private:
    Vector3 goal_;
    Status status_ = Status::Idle;
    static constexpr f32 ARRIVAL_TOLERANCE = 0.5f;
};

} // namespace osc::sim
