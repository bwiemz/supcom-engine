#include "renderer/sky_dome.hpp"

#include "map/scmap_parser.hpp"

#include <cmath>
#include <limits>

namespace osc::renderer {

namespace {

constexpr f32 kHalfPi = 1.5707964f;
constexpr f32 kTwoPi = 6.2831855f;

} // namespace

SkyDomeMesh build_sky_dome(const map::ScmapSky& sky) {
    SkyDomeMesh mesh;
    const i32 width = sky.width;
    const i32 height = sky.height;
    if (width < 1 || height < 1) return mesh;
    const i64 vertex_count = static_cast<i64>(height) * (width + 1) + 1;
    if (vertex_count > std::numeric_limits<u16>::max()) return mesh;
    const f32 radius_div_cos = sky.radius / std::cos(sky.start_angle);
    if (!std::isfinite(radius_div_cos)) return mesh;

    // The rings, from the start angle up; the lowest at the elevation
    const f32 inv_width = 1.0f / static_cast<f32>(width);
    const f32 step = (kHalfPi - sky.start_angle) / static_cast<f32>(height);
    const f32 base_height = radius_div_cos * std::sin(sky.start_angle);
    const f32 lift = sky.origin[1] + sky.elevation;
    mesh.vertices.reserve(static_cast<size_t>(vertex_count));
    for (i32 row = 0; row < height; ++row) {
        const f32 angle = sky.start_angle + static_cast<f32>(row) * step;
        const f32 ring_radius = std::cos(angle) * radius_div_cos;
        const f32 ring_height = std::sin(angle) * radius_div_cos - base_height;
        for (i32 column = 0; column <= width; ++column) {
            const f32 theta = static_cast<f32>(column) * inv_width * kTwoPi;
            mesh.vertices.push_back({{std::cos(theta) * ring_radius + sky.origin[0],
                                      ring_height + lift,
                                      std::sin(theta) * ring_radius + sky.origin[2]},
                                     theta});
        }
    }
    mesh.vertices.push_back(
        {{sky.origin[0], radius_div_cos - base_height + lift, sky.origin[2]}, 0.0f});

    // Each ring pair's quads, then the fan to the apex
    const auto index = [](i64 i) { return static_cast<u16>(i); };
    mesh.indices.reserve(static_cast<size_t>(width) * (6 * (height - 1) + 3));
    for (i32 ring = 0; ring < height - 1; ++ring) {
        const i64 base = static_cast<i64>(ring) * (width + 1);
        for (i32 c = 0; c < width; ++c) {
            // This ring's vertex and the next around, and the two above them
            const i64 here = base + c;
            const i64 next = here + 1;
            const i64 above = here + width + 1;
            const i64 above_next = above + 1;
            for (const i64 i : {next, above, here, above, next, above_next})
                mesh.indices.push_back(index(i));
        }
    }
    const i64 cap = static_cast<i64>(height - 1) * (width + 1);
    const i64 apex = vertex_count - 1;
    for (i32 c = 0; c < width; ++c) {
        for (const i64 i : {cap + c + 1, apex, cap + c}) mesh.indices.push_back(index(i));
    }
    return mesh;
}

} // namespace osc::renderer
