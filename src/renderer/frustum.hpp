#pragma once

#include "core/types.hpp"

#include <array>

namespace osc::renderer {

/// Frustum extracted from a view-projection matrix.
/// Used for visibility culling via bounding sphere tests.
class Frustum {
public:
    /// Extract frustum planes from a column-major VP matrix (Gribb-Hartmann method).
    explicit Frustum(const std::array<f32, 16>& m);

    /// Test if a bounding sphere is at least partially inside the frustum.
    bool is_sphere_visible(f32 cx, f32 cy, f32 cz, f32 radius) const;

    /// Test if an axis-aligned box is at least partially inside the frustum
    /// (no plane has all of it behind): its corner farthest along each
    /// plane's normal is tested.
    bool is_box_visible(const std::array<f32, 3>& min, const std::array<f32, 3>& max) const;

    /// The planes, each (a, b, c, d) with a unit normal and the inside where
    /// a·x + b·y + c·z + d >= 0: left, right, bottom, top, near, far.
    std::array<std::array<f32, 4>, 6> planes() const;

private:
    struct Plane {
        f32 a, b, c, d; // normal (a,b,c) + distance d; normalized
    };
    Plane planes_[6]; // left, right, bottom, top, near, far
};

} // namespace osc::renderer
