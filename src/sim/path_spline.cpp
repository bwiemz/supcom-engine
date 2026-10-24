#include "sim/path_spline.hpp"

#include "core/dmath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace osc::sim::path {

namespace {

// faf-re CAiPathSpline.cpp.
constexpr f32 kDegreesToRadiansPerTick = 0.0017453292f;
constexpr f32 kSpeedPerTick = 0.1f;
constexpr f32 kAccelPerTick = 0.0099999998f;
constexpr f32 kTurnSinePolyA = 0.0076100002f;
constexpr f32 kTurnSinePolyB = 0.16605f;
constexpr f32 kStoppedSpeedRatio = 0.0099999998f;
constexpr f32 kHeadingBlendNew = 0.2f;
constexpr f32 kHeadingBlendOld = 0.80000001f;
constexpr f32 kForwardAlignedCos = 0.866f;
constexpr f32 kReverseAlignedCos = 0.15000001f;

/// x and z scaled to `length`, `length_sq` being their length squared
/// (ScalePlanarToLength).
void scale_to(f32& x, f32& z, f32 length_sq, f32 length) {
    const f32 scale = length / std::sqrt(length_sq);
    x *= scale;
    z *= scale;
}

/// SteeringParams (0x006990E0) in the spline's terms: a tick's speeds and
/// rates, and the destination from `from` in the steering plane, (x, -z).
struct Params {
    f32 max_speed = 0, max_reverse = 0, max_accel = 0, max_brake = 0, max_steer = 0;
    f32 turn_rate = 0;
    f32 turn_radius = 0; // TurnRadius / turnMult; infinite for none
    f32 delta_x = 0, delta_z = 0, distance_sq = 0, distance = 0;
    f32 forward_x = 0, forward_z = 0;
};

Params params_for(const SplineUnit& u, const Vector3& from, const Vector3& to,
                  const Vector3& forward, f32 speed_limit) {
    Params p;
    p.delta_x = to.x - from.x;
    p.delta_z = -(to.z - from.z);
    p.distance_sq = p.delta_x * p.delta_x + p.delta_z * p.delta_z;
    p.distance = std::sqrt(p.distance_sq);
    p.forward_x = forward.x;
    p.forward_z = -forward.z;
    // Wm3's Normalize: times the inverse length; zero at 1e-6 or under.
    const f32 flen = std::sqrt(p.forward_x * p.forward_x + p.forward_z * p.forward_z);
    if (flen > 1.0e-6f) {
        const f32 inv = 1.0f / flen;
        p.forward_x *= inv;
        p.forward_z *= inv;
    } else {
        p.forward_x = 0;
        p.forward_z = 0;
    }
    p.turn_rate = u.turn_rate * u.turn_mult * kDegreesToRadiansPerTick;
    // The limit replaces the blueprint's speed unless it is greater.
    const f32 speed = speed_limit > u.max_speed ? u.max_speed : speed_limit;
    p.max_speed = speed * u.speed_mult * kSpeedPerTick;
    const f32 reverse = speed_limit > u.max_speed_reverse ? u.max_speed_reverse : speed_limit;
    p.max_reverse = reverse * u.speed_mult * kSpeedPerTick;
    p.max_accel = u.max_accel * u.accel_mult * kAccelPerTick;
    p.max_brake = u.max_brake != 0 ? u.max_brake * u.accel_mult * kAccelPerTick : p.max_accel;
    p.max_steer =
        u.max_steer_force != 0 ? u.max_steer_force * u.accel_mult * kAccelPerTick : p.max_accel;
    p.turn_radius =
        u.turn_radius == 0 ? std::numeric_limits<f32>::infinity() : u.turn_radius / u.turn_mult;
    return p;
}

/// BrakingDistanceFor: the distance to stop from a tick's speed, at the greater
/// of its acceleration and its brake.
f32 braking_distance(const SplineUnit& u, f32 speed_per_tick) {
    const f32 decel = u.max_brake > u.max_accel ? u.max_brake : u.max_accel;
    if (decel <= 0) return 0;
    const f32 speed = speed_per_tick * 10.0f;
    return speed * speed / (decel * 2.0f);
}

/// `v` set to `length` (VecSetLength); a zero vector stays zero.
Vector3 with_length(Vector3 v, f32 length) {
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len == 0) return v;
    const f32 scale = length / len;
    return {v.x * scale, v.y * scale, v.z * scale};
}

} // namespace

f32 steering_speed_cap(f32 delta_x, f32 delta_z, f32 forward_x, f32 forward_z, f32 max_speed,
                       f32 turn_rate, f32 turn_radius, bool rotate_on_spot, f32 rotate_threshold,
                       f32 speed_ratio) {
    if (rotate_on_spot && rotate_threshold > speed_ratio) {
        const f32 inv = 1.0f / std::sqrt(delta_x * delta_x + delta_z * delta_z);
        const f32 aligned = forward_z * (delta_z * inv) + forward_x * (delta_x * inv);
        return aligned < 0.98000002f ? 0.0f : max_speed;
    }
    // The radius of the circle through the destination, tangent to the heading.
    const f32 cross = delta_z * forward_x - forward_z * delta_x;
    const f32 circle = cross == 0 ? 0.0f : (delta_x * delta_x + delta_z * delta_z) * 0.5f / cross;
    const f32 abs_circle = std::fabs(circle);
    if (!(abs_circle >= turn_radius)) {
        if (abs_circle == 0) return max_speed;
        return turn_rate * abs_circle * 0.5f;
    }
    return turn_radius < 0 ? 0.0f : turn_radius;
}

void rotate_toward(f32 max_turn, f32 source_x, f32 source_z, f32 target_x, f32 target_z, f32& out_x,
                   f32& out_z) {
    if (!(3.1415927f > max_turn)) max_turn = 3.1415927f;
    const f32 source_len = std::sqrt(source_x * source_x + source_z * source_z);
    const f32 target_len = std::sqrt(target_x * target_x + target_z * target_z);
    const f32 lengths = source_len * target_len;
    const f32 max_cos = osc::dmath::cos(max_turn);
    if (lengths == 0) {
        out_x = source_x;
        out_z = source_z;
        return;
    }
    const f32 dot = target_z * source_z + target_x * source_x;
    if (dot >= max_cos * lengths) {
        const f32 scale = source_len / target_len;
        out_x = target_x * scale;
        out_z = target_z * scale;
        return;
    }
    const f32 cross = target_z * source_x - source_z * target_x;
    const f32 turn_sq = max_turn * max_turn;
    // Moho's polynomial sine of the turn.
    f32 turn_sin = ((turn_sq * kTurnSinePolyA - kTurnSinePolyB) * turn_sq + 1.0f) * max_turn;
    if (!(cross >= 0)) turn_sin = -turn_sin;
    f32 cx = max_cos;
    f32 cz = turn_sin;
    if (std::fabs(turn_sin * turn_sin + max_cos * max_cos - 1.0f) > 0.001f) {
        // Wm3's Normalize: times the inverse length.
        const f32 inv = 1.0f / std::sqrt(cx * cx + cz * cz);
        cx *= inv;
        cz *= inv;
    }
    out_z = cx * source_z + cz * source_x;
    out_x = cx * source_x - cz * source_z;
}

void Spline::brake(const SplineUnit& u, const Pose& pose, int mode, const SplineWorld& world) {
    nodes_.clear();
    type_ = static_cast<PathType>(mode);
    current_ = 0;
    const Vector3 forward = pose.forward;
    Vector3 position = pose.position;
    const Vector3& v = pose.velocity;
    const f32 speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    // Its momentum along its facing, braked; backing, against it.
    Vector3 step = with_length(forward, speed);
    constexpr f32 kMax = std::numeric_limits<f32>::max();
    Vector3 travel{kMax, kMax, kMax};
    // Times the inverse speed, as Update forms it (a quotient rounds otherwise).
    if (speed != 0) {
        const f32 inverse = 1.0f / speed;
        travel = {v.x * inverse, v.y * inverse, v.z * inverse};
    }
    if (travel.y * forward.y + travel.z * forward.z + travel.x * forward.x < 0)
        step = {-step.x, -step.y, -step.z};
    bool done = false;
    while (!done) {
        cont_position_ = position;
        cont_direction_ = forward;
        cont_velocity_ = step;
        cont_state_ = PointState::None;
        const Vector3 target{position.x + travel.x, position.y + travel.y, position.z + travel.z};
        Params p = params_for(u, position, target, travel, u.top_speed);
        if (mode == 4) {
            p.max_accel *= 2.0f;
            p.max_brake *= 2.0f;
        }
        const f32 px = step.x;
        const f32 pz = -step.z;
        f32 bx = -step.x;
        f32 bz = step.z;
        f32 speed_sq = pz * pz + px * px;
        if (speed_sq > 0) {
            const f32 limit = bz * pz + bx * px <= 0 ? p.max_brake : p.max_accel;
            const f32 brake_sq = bz * bz + bx * bx;
            if (brake_sq > limit * limit) scale_to(bx, bz, brake_sq, limit);
            f32 nx = bx + px;
            f32 nz = bz + pz;
            const f32 new_sq = nz * nz + nx * nx;
            if (new_sq > p.max_speed * p.max_speed) scale_to(nx, nz, new_sq, p.max_speed);
            if (nz * nz + nx * nx <= 1.0e-6f) {
                step = {};
                speed_sq = 0;
            } else {
                position.x = nx + position.x;
                position.z = -nz + position.z;
                step = {nx, 0, -nz};
            }
        }
        done = speed_sq <= 1.0e-6f && (mode == 3 || (mode == 4 && nodes_.size() > 5));
        if (world.height) position.y = world.height(position.x, position.z);
        nodes_.push_back({position, forward});
    }
}

void Spline::clear() {
    nodes_.clear();
    current_ = 0;
}

const Node* Spline::advance() {
    if (current_ >= nodes_.size()) return nullptr;
    return &nodes_[current_++];
}

void Spline::generate(const SplineUnit& u, const Pose& pose, const Vector3& destination,
                      PathType type, bool carry_on, bool at_goal, const SplineWorld& world) {
    nodes_.clear();
    type_ = type;
    current_ = 0;
    int limit = u.node_limit;
    // TurnRadius against TurnRate as the blueprint gives them.
    const bool wide = u.turn_radius > u.turn_rate;
    if (wide) limit *= 3;

    bool backup = false;
    Vector3 forward = pose.forward;
    if (std::fabs(forward.y) > 0.99000001f) forward = {0, 0, 1};
    Vector3 position = pose.position;
    f32 dx = destination.x - position.x;
    f32 dz = destination.z - position.z;
    const f32 distance = std::sqrt(dz * dz + dx * dx);
    if (distance < 0.001f) return;
    Vector3 velocity = pose.velocity;
    // Moho's direction to it is the delta times the inverse length (DirectionOfDelta).
    const f32 inv_distance = 1.0f / distance;
    f32 alignment = (inv_distance * dz) * forward.z + (inv_distance * dx) * forward.x;
    const f32 current_speed =
        std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z + velocity.y * velocity.y);
    f32 ratio = current_speed * 10.0f / u.max_speed;

    PointState state = PointState::Forward;
    if (carry_on) {
        position = cont_position_;
        forward = cont_direction_;
        velocity = cont_velocity_;
        if (cont_state_ != PointState::None) state = cont_state_;
    } else if (u.max_speed_reverse > 0 && !u.rotate_on_spot) {
        const bool backward = pose.velocity.z * forward.z + pose.velocity.y * forward.y +
                                  pose.velocity.x * forward.x <
                              0;
        if (alignment < 0 && (ratio < 0.5f || wide)) {
            state = ratio > kStoppedSpeedRatio && !backward ? PointState::BrakeForward
                                                            : PointState::Reverse;
        } else if (backward) {
            state = alignment >= 0 ? PointState::BrakeReverse : PointState::Reverse;
        }
    }
    if (state == PointState::Reverse) {
        backup = u.backup_distance > distance && alignment < -0.5f;
    } else if (state == PointState::Done) {
        return;
    }

    for (;;) {
        cont_position_ = position;
        cont_direction_ = forward;
        cont_velocity_ = velocity;
        cont_state_ = PointState::None;

        // shortcut: an amphibious floater's slowing in shallow water (LayerChangeOffsetHeight)
        // is left out; add it with naval units.
        Params p = params_for(u, position, destination, forward, u.top_speed);
        const f32 vx = velocity.x;
        const f32 vz = -velocity.z;
        f32 speed = std::sqrt(vz * vz + vx * vx);
        f32 turn_limit = speed / p.turn_radius;
        if (p.turn_rate > turn_limit) turn_limit = p.turn_rate;
        if (type == PathType::Sidestep) {
            p.max_accel *= 2.0f;
            p.max_brake *= 2.0f;
            turn_limit *= 2.0f;
        }

        // The heading turned toward the destination; backing up, the tail.
        f32 hx = 0, hz = 0;
        if (backup) {
            rotate_toward(turn_limit, -p.forward_x, -p.forward_z, p.delta_x, p.delta_z, hx, hz);
            hx = -hx;
            hz = -hz;
        } else {
            rotate_toward(turn_limit, p.forward_x, p.forward_z, p.delta_x, p.delta_z, hx, hz);
        }
        forward = {hx, 0, -hz};

        const bool backing = state == PointState::Reverse || state == PointState::BrakeReverse ||
                             state == PointState::Sidestep;
        f32 drive_hx = backing ? -hx : hx;
        f32 drive_hz = backing ? -hz : hz;
        const f32 drive_len_sq = drive_hx * drive_hx + drive_hz * drive_hz;

        // The velocity along the heading, and across it no more than MaxSteerForce.
        f32 along_x = 0, along_z = 0;
        if (drive_len_sq > 0) {
            const f32 projection = (drive_hz * vz + drive_hx * vx) / drive_len_sq;
            along_x = projection * drive_hx;
            along_z = drive_hz * projection;
        }
        f32 side_x = vx - along_x;
        f32 side_z = vz - along_z;
        const f32 side_sq = side_x * side_x + side_z * side_z;
        if (side_sq > p.max_steer * p.max_steer) scale_to(side_x, side_z, side_sq, p.max_steer);
        f32 drive_x = vx - side_x;
        f32 drive_z = vz - side_z;

        f32 want = 0;
        f32 cap = 0;
        bool coasting = state == PointState::Settle || state == PointState::Stop ||
                        state == PointState::BrakeForward || state == PointState::BrakeReverse;
        if (!coasting) {
            f32 c = steering_speed_cap(p.delta_x, p.delta_z, p.forward_x, p.forward_z, p.max_speed,
                                       p.turn_rate, p.turn_radius, u.rotate_on_spot,
                                       u.rotate_threshold, ratio);
            if (state == PointState::Reverse || state == PointState::Sidestep) {
                if (c > p.max_reverse) c = p.max_reverse;
            } else if (c > p.max_speed) {
                c = p.max_speed;
            }
            cap = c;
            want = c;
            if (type == PathType::Normal) {
                // The stopping speed: sqrt(2 d brake), or all of d in a tick.
                f32 stopping = p.distance;
                if (p.max_brake < p.distance)
                    stopping = static_cast<f32>(std::sqrt(
                        static_cast<f64>(p.distance * p.max_brake + p.distance * p.max_brake)));
                if (stopping <= cap) {
                    cap = stopping;
                    want = stopping;
                }
            }
            if (wide) {
                f32 facing = backup ? -alignment : alignment;
                if (facing < -0.5f) facing = -0.5f;
                cap = (facing + 1.0f) * 0.5f * cap;
                want = cap;
            }
            coasting = cap < 0.001f;
        }
        if (coasting) {
            // Braking, the velocity turns a fifth of the way onto the heading.
            f32 blend_x = drive_hx;
            f32 blend_z = drive_hz;
            if (drive_len_sq != 0) {
                const f32 drive_len = std::sqrt(drive_x * drive_x + drive_z * drive_z);
                scale_to(blend_x, blend_z, drive_len_sq, drive_len);
            }
            drive_x = blend_x * kHeadingBlendNew + drive_x * kHeadingBlendOld;
            cap = want;
            drive_z = blend_z * kHeadingBlendNew + drive_z * kHeadingBlendOld;
        }

        // Toward the capped speed along the heading, at its acceleration or brake.
        f32 accel_x = drive_hx * cap - drive_x;
        f32 accel_z = drive_hz * cap - drive_z;
        const f32 accel_limit =
            accel_x * drive_x + accel_z * drive_z <= 0 ? p.max_brake : p.max_accel;
        const f32 accel_sq = accel_x * accel_x + accel_z * accel_z;
        if (accel_sq > accel_limit * accel_limit) scale_to(accel_x, accel_z, accel_sq, accel_limit);
        f32 move_x = accel_x + drive_x;
        f32 move_z = accel_z + drive_z;
        const f32 move_cap = backing ? p.max_reverse : p.max_speed;
        const f32 move_sq = move_x * move_x + move_z * move_z;
        if (move_sq > move_cap * move_cap) scale_to(move_x, move_z, move_sq, move_cap);

        position.x = move_x + position.x;
        position.z = -move_z + position.z;
        velocity = {move_x, 0, -move_z};
        speed = std::sqrt(move_x * move_x + velocity.z * velocity.z);
        ratio = speed / p.max_speed;
        if (world.height) position.y = world.height(position.x, position.z);
        nodes_.push_back({position, forward});

        dx = destination.x - position.x;
        dz = destination.z - position.z;
        const f32 remaining = std::sqrt(dz * dz + dx * dx);
        if (static_cast<int>(nodes_.size()) >= limit || remaining < 0.001f) {
            cont_state_ = state;
            state = PointState::Done;
        }
        if (remaining == 0) {
            alignment = std::numeric_limits<f32>::max() * (forward.x + forward.z);
        } else {
            const f32 inv_remaining = 1.0f / remaining;
            alignment = forward.z * (inv_remaining * dz) + (inv_remaining * dx) * forward.x;
        }
        if (at_goal) state = PointState::Done;

        bool finished = false;
        switch (state) {
        case PointState::Stop: finished = ratio <= kStoppedSpeedRatio; break;
        case PointState::BrakeForward:
            if (ratio > kStoppedSpeedRatio) break;
            state = u.max_speed_reverse <= 0 || u.rotate_on_spot ? PointState::Forward
                                                                 : PointState::Reverse;
            break;
        case PointState::Reverse: {
            const f32 braking = braking_distance(u, speed);
            const Vector3 reach = with_length(velocity, std::max(u.size_x, u.size_z) + braking);
            if (world.wont_fit && world.wont_fit(reach.x + position.x, reach.z + position.z)) {
                state = PointState::BrakeReverse;
                break;
            }
            const bool keep = backup ? braking <= remaining : alignment <= kReverseAlignedCos;
            if (!keep) state = PointState::BrakeReverse;
            break;
        }
        case PointState::BrakeReverse:
            if (ratio > kStoppedSpeedRatio) break;
            if (backup) {
                finished = true;
                break;
            }
            state = PointState::Forward;
            break;
        case PointState::Forward: {
            const f32 braking = braking_distance(u, speed);
            if (type == PathType::Normal && braking > remaining) state = PointState::Stop;
            if (alignment >= kForwardAlignedCos) break;
            const Vector3 reach = with_length(velocity, std::max(u.size_x, u.size_z) + braking);
            const f32 rx = reach.x + position.x;
            const f32 rz = reach.z + position.z;
            if (world.wont_fit && world.wont_fit(rx, rz)) {
                state = PointState::BrakeForward;
            } else if (wide && world.blocked && world.blocked(rx, rz)) {
                state = PointState::BrakeForward;
            }
            break;
        }
        default: finished = state == PointState::Done; break;
        }
        if (finished) break;
    }
}

} // namespace osc::sim::path
