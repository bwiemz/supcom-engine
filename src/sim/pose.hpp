#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3, Quaternion, quat helpers

#include <cmath>
#include <vector>

namespace osc::sim {

/// A bone's transform: in model space (the unit's own frame), or relative
/// to its parent bone.
struct BonePose {
    Vector3 position;
    Quaternion rotation;
};

inline Quaternion quat_conjugate(const Quaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

/// A rotation of `radians` about the bone-local axis 'x', 'y' or 'z'.
inline Quaternion quat_axis_angle(char axis, f32 radians) {
    const f32 s = osc::dmath::sin(radians * 0.5f);
    const f32 c = osc::dmath::cos(radians * 0.5f);
    switch (axis) {
    case 'x': return {s, 0, 0, c};
    case 'z': return {0, 0, s, c};
    default: return {0, s, 0, c};
    }
}

/// `local` placed under `parent`: the child's transform in the parent's space.
inline BonePose pose_compose(const BonePose& parent, const BonePose& local) {
    const Vector3 offset = quat_rotate(parent.rotation, local.position);
    return {
        {parent.position.x + offset.x, parent.position.y + offset.y, parent.position.z + offset.z},
        quat_multiply(parent.rotation, local.rotation)};
}

/// The inverse of pose_compose: `world` expressed relative to `parent`.
inline BonePose pose_relative(const BonePose& parent, const BonePose& world) {
    const Quaternion inverse = quat_conjugate(parent.rotation);
    const Vector3 d{world.position.x - parent.position.x, world.position.y - parent.position.y,
                    world.position.z - parent.position.z};
    return {quat_rotate(inverse, d), quat_multiply(inverse, world.rotation)};
}

/// Normalized linear interpolation of rotations, the short way round.
inline Quaternion quat_nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 sign = dot < 0 ? -1.0f : 1.0f;
    Quaternion r{a.x + t * (sign * b.x - a.x), a.y + t * (sign * b.y - a.y),
                 a.z + t * (sign * b.z - a.z), a.w + t * (sign * b.w - a.w)};
    const f32 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len <= 1e-8f) return a; // near-antipodal: keep the start
    const f32 inv = 1.0f / len;
    return {r.x * inv, r.y * inv, r.z * inv, r.w * inv};
}

inline Vector3 vec3_lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
}

/// Column-major 4x4 matrix of a rotation and a translation.
inline void pose_to_mat4(f32* out, const BonePose& p) {
    const Quaternion& q = p.rotation;
    const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    out[0] = 1.0f - 2.0f * (yy + zz);
    out[1] = 2.0f * (xy + wz);
    out[2] = 2.0f * (xz - wy);
    out[3] = 0.0f;
    out[4] = 2.0f * (xy - wz);
    out[5] = 1.0f - 2.0f * (xx + zz);
    out[6] = 2.0f * (yz + wx);
    out[7] = 0.0f;
    out[8] = 2.0f * (xz + wy);
    out[9] = 2.0f * (yz - wx);
    out[10] = 1.0f - 2.0f * (xx + yy);
    out[11] = 0.0f;
    out[12] = p.position.x;
    out[13] = p.position.y;
    out[14] = p.position.z;
    out[15] = 1.0f;
}

/// 4x4 column-major matrix product: c = a * b.
inline void mat4_multiply(f32* c, const f32* a, const f32* b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            c[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

/// A unit's bones as its manipulators leave them this tick: each bone's
/// transform relative to its parent, starting from the bind pose. The
/// manipulators apply in precedence order, each on top of those before: an
/// animator sets its bones, rotators and aim controllers turn them, sliders
/// move them along their own axes.
struct PoseLocals {
    std::vector<BonePose> local;
    bool changed = false;

    bool valid(i32 bone) const { return bone >= 0 && static_cast<size_t>(bone) < local.size(); }
    void set(i32 bone, const BonePose& p) {
        if (!valid(bone)) return;
        local[static_cast<size_t>(bone)] = p;
        changed = true;
    }
    void rotate(i32 bone, const Quaternion& q) {
        // A turret or rotator at rest leaves its bone alone, so a unit whose
        // manipulators all rest keeps the (cheaper) bind pose.
        if (!valid(bone) || (q.x == 0 && q.y == 0 && q.z == 0 && q.w == 1)) return;
        auto& b = local[static_cast<size_t>(bone)];
        b.rotation = quat_multiply(b.rotation, q);
        changed = true;
    }
    void slide(i32 bone, const Vector3& offset) {
        if (!valid(bone) || (offset.x == 0 && offset.y == 0 && offset.z == 0)) return;
        auto& b = local[static_cast<size_t>(bone)];
        const Vector3 d = quat_rotate(b.rotation, offset);
        b.position = {b.position.x + d.x, b.position.y + d.y, b.position.z + d.z};
        changed = true;
    }
};

} // namespace osc::sim
