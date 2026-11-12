#include "sim/prop.hpp"

#include "core/dmath.hpp"
#include "map/terrain.hpp"
#include "sim/flight_math.hpp"

#include <cmath>

namespace osc::sim {

namespace {

constexpr f32 kPi = 3.1415927f;
constexpr f32 kHalfPi = 1.5707964f;
constexpr f32 kQuarterPi = 0.78539819f;
constexpr f32 kFourOverPi = 1.2732395f;
constexpr f32 kAccelFactor = 0.1f;
constexpr f32 kSpringFactor = 0.5f;
constexpr f32 kDampFactor = 0.5f;
constexpr f32 kUprootFactor = 0.1f;

/// func_NormalizeAngle (0x0062FB50): MSVC8's fmod (-0 as +0), then a turn either way.
f32 signed_angle(f32 a) {
    constexpr f32 kTwoPi = 6.2831855f;
    const f32 wrapped =
        static_cast<f32>(std::fmod(static_cast<double>(a), static_cast<double>(kTwoPi))) + 0.0f;
    if (wrapped < -kPi) {
        return wrapped + kTwoPi;
    }
    if (wrapped > kPi) {
        return wrapped - kTwoPi;
    }
    return wrapped;
}

/// BuildTiltShortestArcDelta (0x0050CB50): the turn taking `up` onto `target`, about their
/// half-vector.
Quaternion tilt_arc(const Vector3& target, const Vector3& up) {
    Vector3 half{up.x + target.x, up.y + target.y, up.z + target.z};
    wm3_normalize(half);
    const f32 w = (up.z * half.z + up.x * half.x) + up.y * half.y;
    if (w == 0.0f) {
        if (!(std::fabs(up.x) >= std::fabs(up.y))) {
            const f32 inv = 1.0f / std::sqrt(up.y * up.y + up.z * up.z);
            return {0.0f, up.z * inv, -(up.y * inv), w};
        }
        const f32 inv = 1.0f / std::sqrt(up.x * up.x + up.z * up.z);
        return {-(up.z * inv), 0.0f, up.x * inv, w};
    }
    return {up.y * half.z - up.z * half.y, up.z * half.x - up.x * half.z,
            up.x * half.y - up.y * half.x, w};
}

} // namespace

void Prop::fall_down(f32 size_x) {
    fall_motor = true;
    fall_breaks = false;
    fall_direction = 0;
    fall_angle = 0;
    fall_speed = 0;
    fall_size_x = size_x;
    wake();
}

void Prop::sink_away(f32 rate) {
    sink_rate = rate;
    fall_motor = false;
    wake();
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
        fall_direction = signed_angle(fall_direction + kPi);
    }
    if (fall_angle > kHalfPi) {
        fall_angle = kHalfPi;
        fall_speed = 0;
    }

    // MotorFallDown::Update (0x00695180): the x87 sines and cosines multiply unrounded.
    const f32 elevation = fall_angle - kHalfPi;
    const double level = dmath::cos(static_cast<double>(elevation));
    const Vector3 target{static_cast<f32>(dmath::sin(static_cast<double>(fall_direction)) * level),
                         -dmath::sin(elevation),
                         static_cast<f32>(dmath::cos(static_cast<double>(fall_direction)) * level)};
    Vector3 p = position();
    const Quaternion& o = orientation();
    const Vector3 up{(o.y * o.x - o.w * o.z) * 2.0f, 1.0f - (o.z * o.z + o.x * o.x) * 2.0f,
                     (o.z * o.y + o.w * o.x) * 2.0f};
    const Quaternion delta = tilt_arc(target, up);
    if (std::fabs(std::fabs(delta.w) - 1.0f) <= 0.0001f) {
        return;
    }

    if (fall_breaks && fall_angle > kQuarterPi && terrain) {
        const f32 rooted = terrain->get_terrain_height(p.x, p.z) + fall_size_x * kUprootFactor;
        p.y += (rooted - p.y) * (fall_angle - kQuarterPi) * kFourOverPi;
    }
    set_position(p);
    set_orientation(quat_normalized_moho(quat_multiply_moho(delta, o)));
}

} // namespace osc::sim
