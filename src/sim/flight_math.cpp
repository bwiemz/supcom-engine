#include "sim/flight_math.hpp"
#include "sim/projectile.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

/// Normalize in place; the length it had
f32 normalize(Vector3& v) {
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 0.0f) {
        v.x /= len;
        v.y /= len;
        v.z /= len;
    }
    return len;
}

} // namespace

Vector3 forward_of(const Quaternion& q) {
    return quat_rotate(q, Vector3{0.0f, 0.0f, 1.0f});
}

Quaternion coords_orient(const Vector3& dir) {
    f32 fx = dir.x, fy = dir.y, fz = dir.z;
    const f32 flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen == 0.0f) return Quaternion{0, 0, 0, 1};
    fx /= flen;
    fy /= flen;
    fz /= flen;
    f32 rx = fz, rz = -fx; // right: level, (forward.z, 0, -forward.x)
    const f32 rlen = std::sqrt(rx * rx + rz * rz);
    if (rlen == 0.0f) {
        constexpr f32 kHalfSqrtTwo = 0.70710677f;
        return Quaternion{fy > 0.0f ? -kHalfSqrtTwo : kHalfSqrtTwo, 0, 0, kHalfSqrtTwo};
    }
    rx /= rlen;
    rz /= rlen;
    const f32 ry = 0.0f;
    const f32 ux = fy * rz - fz * ry, uy = fz * rx - rz * fx, uz = ry * fx - fy * rx;
    // Rows as axes (m): the rotation matrix is its transpose, so
    // x = m12 - m21, y = m20 - m02, z = m01 - m10 (Moho's MatrixToQuat).
    const f32 m[3][3] = {{rx, ry, rz}, {ux, uy, uz}, {fx, fy, fz}};
    const f32 trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const f32 t = std::sqrt(trace + 1.0f) * 2.0f; // 4w
        return Quaternion{(m[1][2] - m[2][1]) / t, (m[2][0] - m[0][2]) / t, (m[0][1] - m[1][0]) / t,
                          0.25f * t};
    }
    // With right level, m11 = |forward.xz| >= 0, and the trace can only
    // fall to zero when forward.z < 0, which makes m00 and m22 negative:
    // m11 is the largest diagonal, so of MatrixToQuat's other branches
    // only the Y one is reachable.
    const f32 t = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f; // 4y
    return Quaternion{(m[0][1] + m[1][0]) / t, 0.25f * t, (m[1][2] + m[2][1]) / t,
                      (m[2][0] - m[0][2]) / t};
}

Quaternion shortest_arc(Vector3 from, Vector3 to) {
    normalize(from);
    normalize(to);
    Vector3 half{to.x + from.x, from.y + to.y, from.z + to.z};
    if (normalize(half) <= 0.0f) {
        normalize(from);
        return Quaternion{from.x, from.y, from.z, 0.0f};
    }
    return Quaternion{(half.z * from.y) - (from.z * half.y), (from.z * half.x) - (half.z * from.x),
                      (half.y * from.x) - (from.y * half.x),
                      (half.x * from.x) + (half.y * from.y) + (half.z * from.z)};
}

Quaternion cap_turn(Quaternion q, f32 angle) {
    constexpr f32 kHalfPi = 1.5707964f;
    const f32 half = std::fabs(angle * 0.5f);
    if (half >= kHalfPi) return q;
    f32 s = osc::dmath::sin(half);
    const f32 axis_sq = (q.z * q.z) + (q.x * q.x) + (q.y * q.y);
    if (axis_sq <= s * s) return q;
    if (q.w < 0.0f) s = -0.0f - s;
    Vector3 axis{q.x, q.y, q.z};
    normalize(axis);
    return Quaternion{axis.x * s, axis.y * s, axis.z * s, osc::dmath::cos(half)};
}

Quaternion turned_toward(const Quaternion& q, const Vector3& toward, f32 angle) {
    const Quaternion turn = cap_turn(shortest_arc(forward_of(q), toward), angle);
    return quat_multiply(turn, q);
}

f32 winged_lift(f32 want, f32 up_y, f32 lift_factor, f32 half_elevation, f32 height) {
    const f32 lift = (up_y - 0.5f) * lift_factor;
    if (lift <= 0.0f) {
        if (half_elevation > height) {
            return half_elevation - height;
        }
        return lift;
    }
    return std::min(want, lift);
}

LiftStep lift_step(f32 velocity, f32 steer, f32 k_lift, f32 k_lift_damping, f32 load, f32 dt) {
    const f32 accel = k_lift_damping * -velocity + steer * (k_lift / load);
    LiftStep out;
    out.velocity = velocity + accel * dt;
    out.rise = (out.velocity + velocity) * (dt * 0.5f);
    return out;
}

f32 next_lift_ground(f32 ground, f32 look_ahead, f32 lift_factor, f32 dt, bool landing) {
    f32 band = lift_factor * dt;
    if (ground > look_ahead && !landing) {
        band *= 0.5f;
    }
    return std::max(ground - band, std::min(ground + band, look_ahead));
}

f32 rising_ground_slowdown(f32 clearance, f32 half_speed) {
    const f32 shrink = std::max(0.2f, (half_speed - clearance) / half_speed);
    return shrink * shrink;
}

f32 air_move_damping(f32 control, f32 top, f32 k_move, f32 k_move_damping) {
    const f32 len = std::min(control, top);
    const f32 denominator = len > 1.0f ? len : 1.0f;
    if (top <= denominator) {
        return k_move;
    }
    return std::min(top / denominator, k_move_damping);
}

AirMoveStep air_move_step(const Vector3& velocity, const Vector3& force, f32 k_move, f32 damping,
                          f32 dt) {
    AirMoveStep out;
    out.velocity.x = velocity.x + (force.x * k_move - velocity.x * damping) * dt;
    out.velocity.z = velocity.z + (force.z * k_move - velocity.z * damping) * dt;
    out.move.x = (out.velocity.x + velocity.x) * (dt * 0.5f);
    out.move.z = (out.velocity.z + velocity.z) * (dt * 0.5f);
    return out;
}

namespace {

Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

f32 dot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 quarter_turn(const Vector3& v) {
    return {v.z, v.y, -v.x};
}

Quaternion conjugate(const Quaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

} // namespace

AirAxes winged_axes(const Vector3& nose, const Vector3& selected, f32 limited, f32 start_turn,
                    f32 turn_speed, f32 bank_factor, f32 elevation_scale) {
    constexpr f32 kPi = 3.1415927f;
    constexpr f32 kTwoPi = 6.2831855f;
    const bool near = start_turn > limited;
    Vector3 planar{selected.x, 0.0f, selected.z};
    normalize(planar);
    const f32 sign = dot(quarter_turn(nose), selected) >= 0.0f ? 1.0f : -1.0f;
    f32 delta = osc::dmath::atan2(planar.x, planar.z) - osc::dmath::atan2(nose.x, nose.z);
    if (delta > kPi) {
        delta -= kTwoPi;
    } else if (delta < -kPi) {
        delta += kTwoPi;
    }
    const f32 max_delta = turn_speed * 0.1f;
    delta = std::clamp(delta, -max_delta, max_delta);
    const f32 turn = delta * 10.0f;
    const f32 c = osc::dmath::cos(turn);
    const f32 s = osc::dmath::sin(turn);
    Vector3 turned{nose.x * c + nose.z * s, selected.y, nose.z * c - nose.x * s};
    normalize(turned);
    const f32 turn_scale = std::min(limited / start_turn, near ? 0.5f : 1.0f);
    const f32 align = std::max(dot(planar, nose), 0.0f);
    const f32 bias = elevation_scale * (1.0f - align) * bank_factor * turn_scale * sign;
    const f32 back = turned.y * turn_scale;
    const Vector3 wing = quarter_turn(turned);
    AirAxes out;
    out.up = {bias * wing.x - planar.x * back, wing.y * bias + 1.0f - planar.y * back,
              bias * wing.z - planar.z * back};
    normalize(out.up);
    out.nose = turned;
    return out;
}

AirAxes hover_axes(const Quaternion& body, Vector3 dv, f32 bank_factor, bool bank_forward,
                   f32 elevation_ratio, const Vector3& facing) {
    if (!bank_forward) {
        const Vector3 f = forward_of(body);
        const f32 len_sq = dot(f, f);
        if (len_sq > 0.0f) {
            const f32 along = dot(f, dv) / len_sq;
            dv.x -= f.x * along;
            dv.y -= f.y * along;
            dv.z -= f.z * along;
        }
    }
    const f32 bank = bank_factor * std::min(elevation_ratio, 1.0f);
    AirAxes out;
    out.up = {dv.x * bank, dv.y * bank + Projectile::GRAVITY * 0.1f, dv.z * bank};
    out.nose = facing;
    return out;
}

Quaternion attitude_of(const AirAxes& axes) {
    Vector3 y = axes.up;
    Vector3 x = cross(y, axes.nose);
    normalize(x);
    Vector3 z = cross(x, y);
    normalize(z);
    y = cross(z, x);
    const f32 m[9] = {x.x, y.x, z.x, x.y, y.y, z.y, x.z, y.z, z.z};
    return rot_matrix_to_quat(m);
}

Vector3 attitude_error(const Quaternion& body, const Quaternion& wanted) {
    Quaternion rel = quat_multiply(conjugate(body), wanted);
    if (rel.w < 0.0f) {
        rel = {-rel.x, -rel.y, -rel.z, -rel.w};
    }
    Vector3 axis{rel.x, rel.y, rel.z};
    if (normalize(axis) <= 0.0f) {
        return {};
    }
    const f32 angle = 2.0f * osc::dmath::acos(std::min(rel.w, 1.0f));
    return {axis.x * angle, axis.y * angle, axis.z * angle};
}

Vector3 to_body(const Quaternion& body, const Vector3& v) {
    return quat_rotate(conjugate(body), v);
}

Vector3 box_inertia(f32 size_x, f32 size_y, f32 size_z) {
    constexpr f32 kOneTwelfth = 0.083333336f;
    const Vector3 out{(size_y * size_y + size_z * size_z) * kOneTwelfth,
                      (size_x * size_x + size_z * size_z) * kOneTwelfth,
                      (size_x * size_x + size_y * size_y) * kOneTwelfth};
    if (out.x <= 0.0f || out.y <= 0.0f || out.z <= 0.0f) {
        return {1.0f, 1.0f, 1.0f};
    }
    return out;
}

Vector3 body_spin(const Quaternion& body, const Vector3& momentum, const Vector3& inertia) {
    const Vector3 local = to_body(body, momentum);
    return {local.x / inertia.x, local.y / inertia.y, local.z / inertia.z};
}

Vector3 momentum_of(const Quaternion& body, const Vector3& spin, const Vector3& inertia) {
    const Vector3 local = to_body(body, spin);
    return quat_rotate(body, {local.x * inertia.x, local.y * inertia.y, local.z * inertia.z});
}

SpinStep air_spin_step(const Quaternion& body, const Vector3& momentum, const Vector3& inertia,
                       const Vector3& accel, f32 dt) {
    const Vector3 push =
        quat_rotate(body, {accel.x * inertia.x, accel.y * inertia.y, accel.z * inertia.z});
    SpinStep out;
    out.momentum = {momentum.x + push.x * dt, momentum.y + push.y * dt, momentum.z + push.z * dt};
    const f32 half = dt * 0.5f;
    const Vector3 avg{(out.momentum.x + momentum.x) * half, (out.momentum.y + momentum.y) * half,
                      (out.momentum.z + momentum.z) * half};
    Vector3 axis = body_spin(body, avg, inertia);
    const f32 angle = normalize(axis);
    Quaternion delta{};
    if (angle > 0.0f) {
        const f32 s = osc::dmath::sin(angle * 0.5f);
        delta = {axis.x * s, axis.y * s, axis.z * s, osc::dmath::cos(angle * 0.5f)};
    }
    const Quaternion q = quat_multiply(body, delta);
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    out.orientation = {q.x / len, q.y / len, q.z / len, q.w / len};
    return out;
}

} // namespace osc::sim
