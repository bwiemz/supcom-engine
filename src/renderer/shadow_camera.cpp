#include "renderer/shadow_camera.hpp"

#include "map/heightmap.hpp"
#include "renderer/camera.hpp"
#include "renderer/frustum.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace osc::renderer {

namespace {

using Vec3 = std::array<f32, 3>;

f32 dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

/// `v` of unit length, or nothing when it is about zero.
std::optional<Vec3> normalized(const Vec3& v) {
    const f32 len = std::sqrt(dot(v, v));
    if (len < 1e-6f) return std::nullopt;
    return Vec3{v[0] / len, v[1] / len, v[2] / len};
}

void grow(Box& box, const Vec3& lo, const Vec3& hi) {
    for (size_t i = 0; i < 3; ++i) {
        box.min[i] = std::min(box.min[i], lo[i]);
        box.max[i] = std::max(box.max[i], hi[i]);
    }
}

// FA's light camera (Shadow::PrepareLightCamera): the eye this far toward
// the sun, the near plane this much nearer than the volume's nearest
// corner, and the volume's top raised by this much.
constexpr f32 kEyeDistance = 10000.0f;
constexpr f32 kNearMargin = 25.0f;
constexpr f32 kTopRaise = 8.0f;

} // namespace

HeightBounds::HeightBounds(const map::Heightmap& heightmap)
    : cells_x_(heightmap.map_width()), cells_z_(heightmap.map_height()) {
    // Level 0: a cell's four corners.
    std::vector<Range> cells(static_cast<size_t>(cells_x_) * cells_z_);
    for (u32 z = 0; z < cells_z_; ++z)
        for (u32 x = 0; x < cells_x_; ++x) {
            const f32 h[4] = {
                heightmap.get_height_at_grid(x, z), heightmap.get_height_at_grid(x + 1, z),
                heightmap.get_height_at_grid(x, z + 1), heightmap.get_height_at_grid(x + 1, z + 1)};
            cells[static_cast<size_t>(z) * cells_x_ + x] = {*std::min_element(h, h + 4),
                                                            *std::max_element(h, h + 4)};
        }
    levels_.push_back(std::move(cells));
    widths_.push_back(cells_x_);
    heights_.push_back(cells_z_);
    // Each level above: a 2 by 2 block of the one below (fewer at its edge).
    while (widths_.back() > 1 || heights_.back() > 1) {
        const u32 w = widths_.back();
        const u32 h = heights_.back();
        const u32 nw = (w + 1) / 2;
        const u32 nh = (h + 1) / 2;
        const std::vector<Range>& below = levels_.back();
        std::vector<Range> above(static_cast<size_t>(nw) * nh);
        for (u32 z = 0; z < nh; ++z)
            for (u32 x = 0; x < nw; ++x) {
                Range r{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::lowest()};
                for (u32 dz = 0; dz < 2; ++dz)
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 cx = x * 2 + dx;
                        const u32 cz = z * 2 + dz;
                        if (cx >= w || cz >= h) continue;
                        const Range& c = below[static_cast<size_t>(cz) * w + cx];
                        r.lo = std::min(r.lo, c.lo);
                        r.hi = std::max(r.hi, c.hi);
                    }
                above[static_cast<size_t>(z) * nw + x] = r;
            }
        levels_.push_back(std::move(above));
        widths_.push_back(nw);
        heights_.push_back(nh);
    }
}

void HeightBounds::visit(u32 level, u32 x, u32 z, const std::vector<Plane>& planes, Box& out,
                         bool& any) const {
    // The node's cells, and its box.
    const u32 span = 1u << level;
    const u32 x0 = x * span;
    const u32 z0 = z * span;
    const u32 x1 = std::min(x0 + span, cells_x_);
    const u32 z1 = std::min(z0 + span, cells_z_);
    if (x0 >= x1 || z0 >= z1) return;
    const Range& r = levels_[level][static_cast<size_t>(z) * widths_[level] + x];
    const Vec3 lo = {static_cast<f32>(x0), r.lo, static_cast<f32>(z0)};
    const Vec3 hi = {static_cast<f32>(x1), r.hi, static_cast<f32>(z1)};
    bool inside = true;
    for (const Plane& p : planes) {
        // The corner farthest inside, and the one farthest out.
        const Vec3 in = {p[0] >= 0.0f ? hi[0] : lo[0], p[1] >= 0.0f ? hi[1] : lo[1],
                         p[2] >= 0.0f ? hi[2] : lo[2]};
        const Vec3 out_corner = {p[0] >= 0.0f ? lo[0] : hi[0], p[1] >= 0.0f ? lo[1] : hi[1],
                                 p[2] >= 0.0f ? lo[2] : hi[2]};
        if (p[0] * in[0] + p[1] * in[1] + p[2] * in[2] + p[3] < 0.0f) return; // wholly outside
        if (p[0] * out_corner[0] + p[1] * out_corner[1] + p[2] * out_corner[2] + p[3] < 0.0f)
            inside = false;
    }
    if (inside || level == 0) {
        grow(out, lo, hi);
        any = true;
        return;
    }
    for (u32 dz = 0; dz < 2; ++dz)
        for (u32 dx = 0; dx < 2; ++dx) {
            const u32 cx = x * 2 + dx;
            const u32 cz = z * 2 + dz;
            if (cx < widths_[level - 1] && cz < heights_[level - 1])
                visit(level - 1, cx, cz, planes, out, any);
        }
}

std::optional<Box> HeightBounds::convex_intersection(const std::vector<Plane>& planes) const {
    if (cells_x_ == 0 || cells_z_ == 0) return std::nullopt;
    Box box;
    box.min = {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
               std::numeric_limits<f32>::max()};
    box.max = {std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(),
               std::numeric_limits<f32>::lowest()};
    bool any = false;
    visit(static_cast<u32>(levels_.size() - 1), 0, 0, planes, box, any);
    if (!any) return std::nullopt;
    return box;
}

std::optional<std::array<f32, 16>> shadow_camera(const ShadowView& view,
                                                 const HeightBounds& bounds) {
    if (view.zoom > kShadowLod) return std::nullopt;

    // The camera's frustum, its far plane ren_ShadowCoeff x zoom past its
    // near plane: the near plane's distance inside, at most that.
    const auto frustum = Frustum(view.view_proj).planes();
    std::vector<Plane> planes(frustum.begin(), frustum.end());
    const Plane& near_plane = planes[4];
    planes[5] = {-near_plane[0], -near_plane[1], -near_plane[2],
                 kShadowCoeff * view.zoom - near_plane[3]};
    std::optional<Box> volume = bounds.convex_intersection(planes);
    if (!volume) return std::nullopt;
    volume->max[1] += kTopRaise;

    // The basis: the light travels along L; up is the camera's forward with
    // its part along L removed, unless the two are nearly parallel.
    const std::optional<Vec3> sun = normalized(view.sun);
    if (!sun) return std::nullopt;
    const Vec3 light = {-(*sun)[0], -(*sun)[1], -(*sun)[2]};
    Vec3 up = {0.0f, 0.0f, -1.0f};
    if (const std::optional<Vec3> forward = normalized(view.forward);
        forward && dot(*forward, light) < 0.99f) {
        const std::optional<Vec3> right = normalized(cross(light, *forward));
        if (right) {
            if (const std::optional<Vec3> u = normalized(cross(*right, light))) up = *u;
        }
    }

    const Vec3 centre = {(volume->min[0] + volume->max[0]) * 0.5f,
                         (volume->min[1] + volume->max[1]) * 0.5f,
                         (volume->min[2] + volume->max[2]) * 0.5f};
    const Vec3 eye = {centre[0] - light[0] * kEyeDistance, centre[1] - light[1] * kEyeDistance,
                      centre[2] - light[2] * kEyeDistance};
    const std::array<f32, 16> light_view =
        math::look_at(eye[0], eye[1], eye[2], centre[0], centre[1], centre[2], up[0], up[1], up[2]);

    // The volume's corners seen from the light.
    Box seen;
    seen.min = {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                std::numeric_limits<f32>::max()};
    seen.max = {std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest(),
                std::numeric_limits<f32>::lowest()};
    for (int i = 0; i < 8; ++i) {
        const Vec3 c = {(i & 1) ? volume->max[0] : volume->min[0],
                        (i & 2) ? volume->max[1] : volume->min[1],
                        (i & 4) ? volume->max[2] : volume->min[2]};
        Vec3 v{};
        for (size_t row = 0; row < 3; ++row)
            v[row] = light_view[row] * c[0] + light_view[4 + row] * c[1] +
                     light_view[8 + row] * c[2] + light_view[12 + row];
        grow(seen, v, v);
    }
    if (seen.max[0] - seen.min[0] < 1e-3f || seen.max[1] - seen.min[1] < 1e-3f) return std::nullopt;
    const f32 near_depth = std::min(std::abs(seen.max[2]), std::abs(seen.min[2])) - kNearMargin;
    const f32 far_depth = std::max(std::abs(seen.max[2]), std::abs(seen.min[2]));

    // D3DXMatrixOrthoOffCenterRH over x and y apart, then y flipped as the
    // main camera's projection flips it.
    std::array<f32, 16> projection =
        math::ortho(seen.min[0], seen.max[0], seen.min[1], seen.max[1], near_depth, far_depth);
    projection[5] = -projection[5];
    projection[13] = -projection[13];
    return math::mat4_mul(projection, light_view);
}

} // namespace osc::renderer
