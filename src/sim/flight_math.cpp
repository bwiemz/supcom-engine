#include "sim/flight_math.hpp"

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

} // namespace osc::sim
