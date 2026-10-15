#include "renderer/world_shapes.hpp"

#include <algorithm>
#include <cmath>

namespace osc::renderer {

namespace {

sim::Vector3 add3(const sim::Vector3& a, const sim::Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

sim::Vector3 sub3(const sim::Vector3& a, const sim::Vector3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

sim::Vector3 scale3(const sim::Vector3& v, f32 s) {
    return {v.x * s, v.y * s, v.z * s};
}

/// `v` at length `length` (none for a zero vector, as Wm3's Normalize)
sim::Vector3 with_length(const sim::Vector3& v, f32 length) {
    const f32 n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 0.0f ? scale3(v, length / n) : sim::Vector3{};
}

} // namespace

f32 WorldShapes::width(const sim::Vector3& p, f32 thickness) const {
    const f32 px = pixel_ ? pixel_(p) : 0.0f;
    if (!(px > 0.0f)) return thickness;
    return std::max(thickness / px, kMinPixels) * px;
}

void WorldShapes::add(const ShapeQuad& q) {
    if (quads_.size() < kMaxQuads) quads_.push_back(q);
}

void WorldShapes::line(const sim::Vector3& a, const sim::Vector3& b, u32 color, f32 thickness) {
    // The farther end's pixel sets the width (the larger LOD metric)
    const f32 px = std::max(pixel_ ? pixel_(a) : 0.0f, pixel_ ? pixel_(b) : 0.0f);
    const f32 full = px > 0.0f ? std::max(thickness / px, kMinPixels) * px : thickness;
    // Across the line, level with the ground (FindNormalVector)
    const sim::Vector3 offset = with_length({-(b.z - a.z), 0.0f, b.x - a.x}, full * 0.5f);
    add({{add3(a, offset), add3(b, offset), sub3(b, offset), sub3(a, offset)}, color});
}

void WorldShapes::circle(const sim::Vector3& centre, f32 radius, u32 color, f32 thickness) {
    const f32 half = width(centre, thickness);
    const f32 inner = radius - half;
    const f32 outer = radius + half;
    // DRAW_Circle's axes for an upward normal: x, and -z
    constexpr f32 kStep = 6.28318530718f / static_cast<f32>(kCircleSegments);
    const auto at = [&](f32 r, u32 segment) {
        const f32 angle = static_cast<f32>(segment) * kStep;
        return sim::Vector3{centre.x + r * std::cos(angle), centre.y,
                            centre.z - r * std::sin(angle)};
    };
    for (u32 s = 0; s < kCircleSegments; ++s) {
        const u32 next = (s + 1) % kCircleSegments;
        add({{at(inner, s), at(outer, s), at(outer, next), at(inner, next)}, color});
    }
}

void WorldShapes::rect(const sim::Vector3& corner, f32 size, u32 color, f32 thickness) {
    // DRAW_Rect with no height field: its four border quads
    const f32 half = width(corner, thickness) * 0.5f;
    const sim::Vector3 height_axis{0.0f, 0.0f, size};
    const sim::Vector3 width_axis{size, 0.0f, 0.0f};
    const sim::Vector3 outer = with_length(add3(height_axis, width_axis), half);
    const sim::Vector3 edge = with_length(sub3(height_axis, width_axis), half);
    const sim::Vector3 far = add3(add3(corner, width_axis), height_axis);
    const sim::Vector3 top_outer_left = sub3(corner, outer);
    const sim::Vector3 top_outer_right = sub3(add3(corner, width_axis), edge);
    const sim::Vector3 bottom_outer_right = add3(far, outer);
    const sim::Vector3 bottom_outer_left = add3(add3(corner, height_axis), edge);
    const sim::Vector3 top_inner_left = add3(corner, outer);
    const sim::Vector3 top_inner_right = add3(add3(corner, width_axis), edge);
    const sim::Vector3 bottom_inner_right = sub3(far, outer);
    const sim::Vector3 bottom_inner_left = sub3(add3(corner, height_axis), edge);
    add({{top_outer_left, top_inner_left, top_inner_right, top_outer_right}, color});
    add({{top_outer_right, top_inner_right, bottom_inner_right, bottom_outer_right}, color});
    // (faf-re lists this one's corners crossed, inner-left, outer-right,
    // outer-left, inner-right, which as DrawQuad's two triangles leaves a
    // hole in the edge; drawn round its ring, as the other three)
    add({{bottom_inner_left, bottom_inner_right, bottom_outer_right, bottom_outer_left}, color});
    add({{top_outer_left, bottom_outer_left, bottom_inner_left, top_inner_left}, color});
}

} // namespace osc::renderer
