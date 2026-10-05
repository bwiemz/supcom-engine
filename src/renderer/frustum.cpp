#include "renderer/frustum.hpp"

#include <cmath>

namespace osc::renderer {

Frustum::Frustum(const std::array<f32, 16>& m) {
    // Gribb-Hartmann frustum plane extraction from column-major VP matrix.
    // Column-major layout: m[col*4+row]
    // m[0..3] = column 0, m[4..7] = column 1, etc.

    // Left:   row3 + row0
    planes_[0] = {m[3] + m[0], m[7] + m[4], m[11] + m[8],  m[15] + m[12]};
    // Right:  row3 - row0
    planes_[1] = {m[3] - m[0], m[7] - m[4], m[11] - m[8],  m[15] - m[12]};
    // Bottom: row3 + row1
    planes_[2] = {m[3] + m[1], m[7] + m[5], m[11] + m[9],  m[15] + m[13]};
    // Top:    row3 - row1
    planes_[3] = {m[3] - m[1], m[7] - m[5], m[11] - m[9],  m[15] - m[13]};
    // Near:   row2 only (Vulkan [0,1] depth convention, not OpenGL [-1,1])
    planes_[4] = {m[2], m[6], m[10], m[14]};
    // Far:    row3 - row2
    planes_[5] = {m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]};

    // Normalize each plane
    for (auto& p : planes_) {
        f32 len = std::sqrt(p.a * p.a + p.b * p.b + p.c * p.c);
        if (len > 1e-8f) {
            f32 inv = 1.0f / len;
            p.a *= inv;
            p.b *= inv;
            p.c *= inv;
            p.d *= inv;
        }
    }
}

bool Frustum::is_sphere_visible(f32 cx, f32 cy, f32 cz, f32 radius) const {
    for (const auto& p : planes_) {
        f32 dist = p.a * cx + p.b * cy + p.c * cz + p.d;
        if (dist < -radius) {
            return false; // entirely outside this plane
        }
    }
    return true;
}

std::array<std::array<f32, 4>, 6> Frustum::planes() const {
    std::array<std::array<f32, 4>, 6> out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {planes_[i].a, planes_[i].b, planes_[i].c, planes_[i].d};
    return out;
}

bool Frustum::is_box_visible(const std::array<f32, 3>& min, const std::array<f32, 3>& max) const {
    for (const auto& p : planes_) {
        const f32 x = p.a >= 0.0f ? max[0] : min[0];
        const f32 y = p.b >= 0.0f ? max[1] : min[1];
        const f32 z = p.c >= 0.0f ? max[2] : min[2];
        if (p.a * x + p.b * y + p.c * z + p.d < 0.0f) return false;
    }
    return true;
}

} // namespace osc::renderer
