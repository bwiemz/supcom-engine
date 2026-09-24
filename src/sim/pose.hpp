#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3, Quaternion, quat helpers

#include <vector>

namespace osc::sim {

/// A bone's transform in model space (the unit's own frame).
struct BonePose {
    Vector3 position;
    Quaternion rotation;
};

/// What the manipulators do to a unit's bones this tick, relative to the
/// bind pose: a rotation in each bone's own frame and a slide along its own
/// axes. A higher-precedence manipulator replaces a lower one's delta on the
/// same bone, as Moho's precedence overrides rather than stacks.
struct PoseDeltas {
    std::vector<Quaternion> rotation;
    std::vector<Vector3> offset;
    std::vector<u8> rotated;
    std::vector<u8> offset_set;

    explicit PoseDeltas(size_t bones)
        : rotation(bones), offset(bones), rotated(bones, 0), offset_set(bones, 0) {}

    void rotate(i32 bone, const Quaternion& q) {
        if (bone < 0 || static_cast<size_t>(bone) >= rotation.size()) return;
        rotation[static_cast<size_t>(bone)] = q;
        rotated[static_cast<size_t>(bone)] = 1;
    }
    void slide(i32 bone, const Vector3& v) {
        if (bone < 0 || static_cast<size_t>(bone) >= offset.size()) return;
        offset[static_cast<size_t>(bone)] = v;
        offset_set[static_cast<size_t>(bone)] = 1;
    }
    bool any() const {
        for (size_t i = 0; i < rotated.size(); ++i)
            if (rotated[i] || offset_set[i]) return true;
        return false;
    }
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

} // namespace osc::sim
