#include "sim/prop.hpp"

#include "core/dmath.hpp"
#include "map/terrain.hpp"

#include <cmath>

namespace osc::sim {

namespace {

constexpr f32 kPi = 3.14159265f;
constexpr f32 kHalfPi = kPi / 2;
constexpr f32 kQuarterPi = kPi / 4;
constexpr f32 kAccelFactor = 0.1f;
constexpr f32 kSpringFactor = 0.5f;
constexpr f32 kDampFactor = 0.5f;
constexpr f32 kUprootFactor = 0.1f;

Vector3 normalized(const Vector3& v, f32& length) {
    length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length <= 1e-6f) {
        return v;
    }
    return {v.x / length, v.y / length, v.z / length};
}

Quaternion arc_between(const Vector3& from_raw, const Vector3& to_raw) {
    f32 from_length = 0;
    f32 to_length = 0;
    const Vector3 to = normalized(to_raw, to_length);
    const Vector3 from = normalized(from_raw, from_length);
    if (to_length <= 1e-6f || from_length <= 1e-6f) {
        return {};
    }
    const f32 dot = from.x * to.x + from.y * to.y + from.z * to.z;
    if (dot < -0.9999f) {
        Vector3 axis{0, from.z, -from.y};
        if (axis.y * axis.y + axis.z * axis.z <= 1e-6f) {
            axis = {from.y, -from.x, 0};
        }
        f32 axis_length = 0;
        axis = normalized(axis, axis_length);
        return {axis.x, axis.y, axis.z, 0};
    }
    Quaternion q{from.y * to.z - from.z * to.y, from.z * to.x - from.x * to.z,
                 from.x * to.y - from.y * to.x, 1.0f + dot};
    const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

} // namespace

void Prop::fall_down(f32 size_x) {
    fall_motor = true;
    fall_breaks = false;
    fall_direction = 0;
    fall_angle = 0;
    fall_speed = 0;
    fall_size_x = size_x;
}

void Prop::whack(f32 nx, f32 nz, f32 force, bool breaks) {
    if (!fall_breaks) {
        fall_direction = dmath::atan2(nx, nz);
        fall_breaks = breaks;
    }
    fall_speed += force;
}

void Prop::step_fall(const map::Terrain* terrain) {
    if (!fall_motor) {
        return;
    }
    const f32 previous = fall_angle;
    if (fall_breaks) {
        fall_speed += kAccelFactor * previous;
        fall_angle = fall_speed + previous;
    } else {
        fall_speed -= previous * kSpringFactor;
        fall_angle = fall_speed + previous;
        fall_speed *= 1.0f - kDampFactor;
    }
    if (fall_angle < 0) {
        fall_angle = -fall_angle;
        fall_speed = -fall_speed;
        fall_direction += kPi;
        while (fall_direction >= 2 * kPi) {
            fall_direction -= 2 * kPi;
        }
        while (fall_direction < 0) {
            fall_direction += 2 * kPi;
        }
    }
    if (fall_angle > kHalfPi) {
        fall_angle = kHalfPi;
        fall_speed = 0;
    }

    const f32 elevation = fall_angle - kHalfPi;
    const f32 level = dmath::cos(elevation);
    const Vector3 target{dmath::sin(fall_direction) * level, -dmath::sin(elevation),
                         dmath::cos(fall_direction) * level};
    const Quaternion delta = arc_between(quat_rotate(orientation(), {0, 1, 0}), target);
    if (std::fabs(std::fabs(delta.w) - 1.0f) <= 0.0001f) {
        return;
    }

    if (fall_breaks && fall_angle > kQuarterPi && terrain) {
        Vector3 p = position();
        const f32 rooted = terrain->get_terrain_height(p.x, p.z) + fall_size_x * kUprootFactor;
        p.y += (rooted - p.y) * (fall_angle - kQuarterPi) * (4 / kPi);
        set_position(p);
    }
    Quaternion q = quat_multiply(delta, orientation());
    const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    set_orientation({q.x / n, q.y / n, q.z / n, q.w / n});
}

} // namespace osc::sim
