#include "sim/flight_math.hpp"
#include "sim/pose.hpp"
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

Vector3 forward_moho(const Quaternion& q) {
    return {(q.z * q.x + q.w * q.y) * 2.0f, (q.y * q.z - q.w * q.x) * 2.0f,
            1.0f - (q.x * q.x + q.y * q.y) * 2.0f};
}

f32 wm3_normalize(Vector3& v) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 1.0e-6f)) {
        v = {};
        return 0.0f;
    }
    const f32 inv = 1.0f / length;
    v = {v.x * inv, v.y * inv, v.z * inv};
    return length;
}

Quaternion coords_orient_moho(const Vector3& dir) {
    Vector3 f = dir;
    if (wm3_normalize(f) == 0.0f) return Quaternion{0, 0, 0, 1};
    Vector3 r{f.z, 0.0f, -f.x};
    if (wm3_normalize(r) == 0.0f) {
        constexpr f32 kHalfSqrtTwo = 0.70710677f;
        return Quaternion{dir.y > 0.0f ? -kHalfSqrtTwo : kHalfSqrtTwo, 0, 0, kHalfSqrtTwo};
    }
    const Vector3 u{f.y * r.z - f.z * r.y, f.z * r.x - r.z * f.x, r.y * f.x - f.y * r.x};
    const f32 m[3][3] = {{r.x, r.y, r.z}, {u.x, u.y, u.z}, {f.x, f.y, f.z}};
    return matrix_to_quat_moho(m);
}

Quaternion matrix_to_quat_moho(const f32 m[3][3]) {
    const f32 trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const f32 root = std::sqrt(trace + 1.0f);
        const f32 inv = 0.5f / root;
        return Quaternion{(m[1][2] - m[2][1]) * inv, (m[2][0] - m[0][2]) * inv,
                          (m[0][1] - m[1][0]) * inv, root * 0.5f};
    }
    constexpr int kNext[3] = {1, 2, 0};
    int d = m[1][1] > m[0][0] ? 1 : 0;
    if (m[2][2] > m[d][d]) {
        d = 2;
    }
    const int b = kNext[d];
    const int c = kNext[b];
    const f32 root = std::sqrt(m[d][d] - (m[b][b] + m[c][c]) + 1.0f);
    const f32 inv = 0.5f / root;
    f32 v[3] = {};
    v[d] = root * 0.5f;
    v[b] = (m[d][b] + m[b][d]) * inv;
    v[c] = (m[d][c] + m[c][d]) * inv;
    return Quaternion{v[0], v[1], v[2], (m[b][c] - m[c][b]) * inv};
}

Quaternion map_prop_orientation(const f32 rows[3][3]) {
    const Quaternion q = matrix_to_quat_moho(rows);
    const f32 length = std::sqrt(q.z * q.z + q.y * q.y + q.x * q.x + q.w * q.w);
    if (!(length > 1.0e-6f)) {
        return q;
    }
    const f32 inv = 1.0f / length;
    return Quaternion{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Quaternion matrix_rows_to_quat_canonical(const f32 m[3][3]) {
    const f32 trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const f32 root = std::sqrt(trace + 1.0f);
        const f32 inv = 0.5f / root;
        return Quaternion{(m[2][1] - m[1][2]) * inv, (m[0][2] - m[2][0]) * inv,
                          (m[1][0] - m[0][1]) * inv, root * 0.5f};
    }
    constexpr int kNext[3] = {1, 2, 0};
    int d = m[1][1] > m[0][0] ? 1 : 0;
    if (m[2][2] > m[d][d]) {
        d = 2;
    }
    const int b = kNext[d];
    const int c = kNext[b];
    const f32 root = std::sqrt(m[d][d] - m[b][b] - m[c][c] + 1.0f);
    const f32 inv = 0.5f / root;
    f32 v[3] = {};
    v[d] = root * 0.5f;
    v[b] = (m[d][b] + m[b][d]) * inv;
    v[c] = (m[d][c] + m[c][d]) * inv;
    return Quaternion{v[0], v[1], v[2], (m[c][b] - m[b][c]) * inv};
}

Quaternion hpr_orientation(f32 heading, f32 pitch, f32 roll) {
    const f32 ch = osc::dmath::cos(heading), sh = osc::dmath::sin(heading);
    const f32 cp = osc::dmath::cos(pitch), sp = osc::dmath::sin(pitch);
    const f32 cr = osc::dmath::cos(roll), sr = osc::dmath::sin(roll);
    const f32 h[3][3] = {{ch, 0.0f, sh}, {0.0f, 1.0f, 0.0f}, {-sh, 0.0f, ch}};
    const f32 p[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, cp, -sp}, {0.0f, sp, cp}};
    const f32 r[3][3] = {{cr, -sr, 0.0f}, {sr, cr, 0.0f}, {0.0f, 0.0f, 1.0f}};
    const auto times = [](const f32 a[3][3], const f32 b[3][3], f32 out[3][3]) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                out[i][j] = ((a[i][0] * b[0][j] + 0.0f) + a[i][1] * b[1][j]) + a[i][2] * b[2][j];
            }
        }
    };
    f32 pr[3][3];
    times(p, r, pr);
    f32 m[3][3];
    times(h, pr, m);
    return matrix_rows_to_quat_canonical(m);
}

Quaternion quat_from_axis_angle_moho(Vector3 axis_angle) {
    const f32 half = wm3_normalize(axis_angle) * 0.5f;
    const f32 s = osc::dmath::sin(half);
    return Quaternion{axis_angle.x * s, axis_angle.y * s, axis_angle.z * s, osc::dmath::cos(half)};
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

namespace {

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
                    f32 turn_speed, f32 bank_factor, f32 elevation_scale, bool normal_turn) {
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
    f32 align = dot(planar, nose);
    if (normal_turn) {
        const f32 sq = align * align;
        align = sq * sq * sq * sq;
        bank_factor *= 10.0f;
    }
    align = std::max(align, 0.0f);
    const f32 bias = elevation_scale * (1.0f - align) * bank_factor * turn_scale * sign;
    const f32 back = turned.y * turn_scale;
    const Vector3 wing = quarter_turn(turned);
    AirAxes out;
    out.up = {bias * wing.x - planar.x * back, wing.y * bias + 1.0f - planar.y * back,
              bias * wing.z - planar.z * back};
    normalize(out.up);
    out.nose = turned;
    out.wing_blend = 1.0f - align;
    return out;
}

AirAxes hover_axes(const Quaternion& body, Vector3 dv, f32 bank_factor, bool bank_forward,
                   f32 elevation_ratio, const Vector3& facing) {
    if (!bank_forward) {
        const Vector3 f = forward_moho(body);
        const f32 len_sq = (f.z * f.z + f.y * f.y) + f.x * f.x;
        if (len_sq > 0.0f) {
            const f32 along = ((f.z * dv.z + f.y * dv.y) + f.x * dv.x) / len_sq;
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
    const Vector3& y = axes.up;
    const Vector3& z = axes.nose;
    Vector3 vx{z.z * y.y - z.y * y.z, z.x * y.z - y.x * z.z, y.x * z.y - z.x * y.y};
    wm3_normalize(vx);
    Vector3 vz{vx.y * y.z - vx.z * y.y, y.x * vx.z - vx.x * y.z, vx.x * y.y - y.x * vx.y};
    wm3_normalize(vz);
    const Vector3 vy{vz.y * vx.z - vz.z * vx.y, vz.z * vx.x - vz.x * vx.z,
                     vz.x * vx.y - vz.y * vx.x};
    const f32 rows[3][3] = {{vx.x, vy.x, vz.x}, {vx.y, vy.y, vz.y}, {vx.z, vy.z, vz.z}};
    return matrix_rows_to_quat_canonical(rows);
}

namespace {

/// The MSVC8 CRT's x87 acos under the sim's 24-bit precision control: atan2 of
/// sqrt((1 + x)(1 - x)) and x, the root's steps rounded to float.
f32 acos_x87(f32 x) {
    const f32 root = std::sqrt((1.0f + x) * (1.0f - x));
    return static_cast<f32>(osc::dmath::atan2(static_cast<double>(root), static_cast<double>(x)));
}

} // namespace

Vector3 attitude_error(const Quaternion& body, const Quaternion& wanted) {
    const Quaternion c = conjugate(body);
    const Quaternion& d = wanted;
    Quaternion rel{((c.x * d.w + d.x * c.w) + d.z * c.y) - d.y * c.z,
                   ((c.y * d.w + d.y * c.w) + d.x * c.z) - d.z * c.x,
                   ((c.z * d.w + d.z * c.w) + d.y * c.x) - d.x * c.y,
                   ((d.w * c.w - d.x * c.x) - d.y * c.y) - d.z * c.z};
    if (0.0f > rel.w) {
        rel = {-rel.x, -rel.y, -rel.z, -rel.w};
    }
    const f32 len_sq = (rel.x * rel.x + rel.y * rel.y) + rel.z * rel.z;
    if (!(len_sq > 1.0e-6f)) {
        return {};
    }
    f32 half = 0.0f;
    if (!(rel.w > -1.0f)) {
        half = 3.1415927f;
    } else if (rel.w < 1.0f) {
        half = acos_x87(rel.w);
    }
    const f32 angle = half * 2.0f;
    const f32 inv = 1.0f / std::sqrt(len_sq);
    return {rel.x * inv * angle, rel.y * inv * angle, rel.z * inv * angle};
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

BodyMass body_mass(f32 density, f32 size_x, f32 size_y, f32 size_z, const Vector3& inertia) {
    Vector3 tensor = inertia;
    if (tensor.x * tensor.y * tensor.z == 0.0f) {
        tensor = box_inertia(size_x, size_y, size_z);
    }
    BodyMass out;
    out.mass = density * size_z * size_x * size_y;
    out.inv_inertia = {1.0f / (tensor.x * out.mass), 1.0f / (tensor.y * out.mass),
                       1.0f / (tensor.z * out.mass)};
    return out;
}

Vector3 body_spin(const Quaternion& body, const Vector3& impulse, const Vector3& inv_inertia) {
    const Vector3 local = mult_quad_vec(conjugate(body), impulse);
    return {inv_inertia.x * local.x, inv_inertia.y * local.y, inv_inertia.z * local.z};
}

Vector3 momentum_of(const Quaternion& body, const Vector3& spin, const Vector3& inv_inertia) {
    const Vector3 local = mult_quad_vec(conjugate(body), spin);
    return mult_quad_vec(
        body, {local.x / inv_inertia.x, local.y / inv_inertia.y, local.z / inv_inertia.z});
}

AirControl air_control_of(const PhysBody& body, const Quaternion& wanted, const Vector3& steer,
                          const AirGains& g) {
    const Quaternion& q = body.orientation;
    const Vector3& inv = body.mass.inv_inertia;
    const Vector3 rot = attitude_error(q, wanted);
    const Vector3 local = mult_quad_vec(conjugate(q), body.impulse);
    const Vector3 damp{-(inv.x * local.x), -(inv.y * local.y), -(inv.z * local.z)};
    const Vector3 v{-body.velocity.x, -body.velocity.y, -body.velocity.z};
    const f32 ax = steer.x * g.k_move + v.x * g.damping;
    const f32 ay = g.k_lift_damping * v.y + steer.y * g.lift;
    const f32 az = steer.z * g.k_move + v.z * g.damping;
    const f32 m = body.mass.mass;
    AirControl out;
    out.force = {(ax - 0.0f) * m, (ay - -Projectile::GRAVITY) * m, (az - 0.0f) * m};
    const Vector3 raw{damp.x * g.k_turn_damping + rot.x * g.turn,
                      damp.y * g.k_turn_damping + rot.y * g.turn,
                      g.k_roll_damping * damp.z + rot.z * g.roll};
    out.torque = mult_quad_vec(q, {raw.x / inv.x, raw.y / inv.y, raw.z / inv.z});
    return out;
}

void integrate_freefall(PhysBody& body, const Vector3& force, const Vector3& torque, f32 dt) {
    const Vector3 was = body.velocity;
    const f32 per_mass = dt / body.mass.mass;
    const f32 fall = -Projectile::GRAVITY * dt;
    body.velocity.x = was.x + (force.x * per_mass + 0.0f * dt);
    body.velocity.y = was.y + (force.y * per_mass + fall);
    body.velocity.z = was.z + (force.z * per_mass + 0.0f * dt);
    const f32 half = dt * 0.5f;
    body.centre.x += (body.velocity.x + was.x) * half;
    body.centre.y += (body.velocity.y + was.y) * half;
    body.centre.z += (body.velocity.z + was.z) * half;
    const SpinStep spin = integrate_angular_impulse(body.orientation, body.impulse,
                                                    body.mass.inv_inertia, torque, dt);
    body.orientation = spin.orientation;
    body.impulse = spin.momentum;
}

SpinStep integrate_angular_impulse(const Quaternion& body, const Vector3& impulse,
                                   const Vector3& inv_inertia, const Vector3& torque, f32 dt) {
    SpinStep out;
    out.momentum = {torque.x * dt + impulse.x, torque.y * dt + impulse.y,
                    torque.z * dt + impulse.z};
    const f32 half = dt * 0.5f;
    const Vector3 mean{(out.momentum.x + impulse.x) * half, (out.momentum.y + impulse.y) * half,
                       (out.momentum.z + impulse.z) * half};
    const Vector3 local = mult_quad_vec(conjugate(body), mean);
    const Quaternion d = quat_from_axis_angle_moho(
        {inv_inertia.x * local.x, inv_inertia.y * local.y, inv_inertia.z * local.z});
    const Quaternion& o = body;
    out.orientation = quat_normalized_moho({((o.y * d.z + o.w * d.x) + o.x * d.w) - o.z * d.y,
                                            ((o.z * d.x + o.w * d.y) + o.y * d.w) - o.x * d.z,
                                            ((o.x * d.y + o.w * d.z) + o.z * d.w) - o.y * d.x,
                                            ((o.w * d.w - o.x * d.x) - o.y * d.y) - o.z * d.z});
    return out;
}

Vector3 tumble_range(const Vector3& inv_inertia) {
    // comiss against 4 then 0.25: a NaN passes the cap and is floored.
    const auto clamp_axis = [](f32 inverse) {
        const f32 capped = inverse > 4.0f ? 4.0f : inverse;
        return capped > 0.25f ? capped : 0.25f;
    };
    return {clamp_axis(inv_inertia.x), clamp_axis(inv_inertia.y), clamp_axis(inv_inertia.z)};
}

Vector3 tumble_torque(const Quaternion& body, const Vector3& inv_inertia, const Vector3& drawn) {
    return mult_quad_vec(
        body, {drawn.x / inv_inertia.x, drawn.y / inv_inertia.y, drawn.z / inv_inertia.z});
}

} // namespace osc::sim
