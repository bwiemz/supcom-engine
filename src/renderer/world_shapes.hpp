#pragma once

// FAF's world drawing (FA-Binary-Patches DrawFunc.cpp and HDraw.cpp, over
// Moho's DRAW_Circle, DRAW_Rect and CD3DPrimBatcher::DrawQuad, faf-re): what
// a world view's OnRenderWorld draws with UI_DrawLine, UI_DrawCircle and
// UI_DrawRect, flat in the world and over it (primbatcher.fx's
// TAlphaBlendLinearSampleNoDepth). Each thickness is at least two pixels:
// max(thickness / px, 2) px, px the world's size of a pixel at the shape's
// depth (GeomCamera3's viewport.r[2], the LOD metric over the view's width).

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <array>
#include <functional>
#include <vector>

namespace osc::renderer {

/// The UI state's registry key of the shapes being drawn: set only while a
/// world view's OnRenderWorld runs, so UI_Draw* outside it is an error.
inline constexpr const char* kWorldShapesKey = "__osc_world_shapes";

/// One flat quad in the world: its corners round its edge, and its colour
/// (packed ARGB, as SCR_DecodeColor gives it).
struct ShapeQuad {
    std::array<sim::Vector3, 4> corners;
    u32 color = 0;
};

/// The shapes one frame's OnRenderWorld draws.
class WorldShapes {
public:
    /// The world's size of a pixel at a point
    using PixelSize = std::function<f32(const sim::Vector3&)>;

    /// A new frame's shapes, none yet, at the view's pixel size.
    void begin(PixelSize pixel) {
        quads_.clear();
        pixel_ = std::move(pixel);
    }

    /// UI_DrawLine(pos1, pos2, color, thickness): a band from `a` to `b`, as
    /// wide as the thickness (at least two pixels at its farther end).
    void line(const sim::Vector3& a, const sim::Vector3& b, u32 color, f32 thickness);
    /// UI_DrawCircle(pos, radius, color, thickness): a ring of 48 segments
    /// level with the ground, the thickness (at least two pixels) either side
    /// of the radius (HDraw's THICKNESS for DRAW_Oval's band).
    void circle(const sim::Vector3& centre, f32 radius, u32 color, f32 thickness);
    /// UI_DrawRect(pos, size, color, thickness): DRAW_Rect's border round the
    /// square from `corner` to corner + (size, 0, size), the thickness (at
    /// least two pixels) wide across its edges, mitred at its corners.
    void rect(const sim::Vector3& corner, f32 size, u32 color, f32 thickness);
    /// DRAW_Rect's border round the box from `corner` to corner + (size_x,
    /// 0, size_z): a rect of any aspect, mitred alike at its corners.
    void box(const sim::Vector3& corner, f32 size_x, f32 size_z, u32 color, f32 thickness);

    const std::vector<ShapeQuad>& quads() const { return quads_; }

    /// HDraw's segments for a circle (0x30), from DRAW_Oval's
    static constexpr u32 kCircleSegments = 48;
    /// The thickness when none is given
    static constexpr f32 kDefaultThickness = 0.15f;
    /// The least thickness, in pixels
    static constexpr f32 kMinPixels = 2.0f;
    /// A frame's quads at most (the rest dropped)
    static constexpr u32 kMaxQuads = 32768;

private:
    /// The thickness at `p`: at least two pixels there
    f32 width(const sim::Vector3& p, f32 thickness) const;
    void add(const ShapeQuad& q);

    PixelSize pixel_;
    std::vector<ShapeQuad> quads_;
};

} // namespace osc::renderer
