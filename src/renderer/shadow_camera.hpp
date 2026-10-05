#pragma once

#include "core/types.hpp"

#include <array>
#include <optional>
#include <vector>

namespace osc::map {
class Heightmap;
}

namespace osc::renderer {

/// Moho's shadow constants (M210c): ren_ShadowSize, ren_ShadowLOD,
/// ren_ShadowCoeff, ren_ShadowBias (faf-re RuntimeTuningGlobals, Mesh.cpp).
inline constexpr u32 kShadowSize = 1024;
inline constexpr f32 kShadowLod = 250.0f;  ///< no shadow camera past this target zoom
inline constexpr f32 kShadowCoeff = 3.0f;  ///< the volume ends this many zooms past the near plane
inline constexpr f32 kShadowBias = 0.005f; ///< the meshes' tap, in light-space depth

/// A plane a·x + b·y + c·z + d, its normal (a, b, c) of unit length, the
/// inside where it is >= 0 (Frustum's convention).
using Plane = std::array<f32, 4>;

/// A world box.
struct Box {
    std::array<f32, 3> min{};
    std::array<f32, 3> max{};
};

/// The heightfield's min/max quadtree (STIMap's tiers): each node holds the
/// lowest and highest height of the cells it covers, a cell the square
/// between four grid points.
class HeightBounds {
public:
    explicit HeightBounds(const map::Heightmap& heightmap);

    /// STIMap::ConvexIntersection: the box of the heightfield cells inside
    /// the convex solid the planes bound. A node wholly inside is taken whole;
    /// one the solid's boundary crosses is split down to single cells, each
    /// taken whole (so the box is conservative). Empty when none is inside.
    std::optional<Box> convex_intersection(const std::vector<Plane>& planes) const;

    u32 cells_x() const { return cells_x_; }
    u32 cells_z() const { return cells_z_; }

private:
    struct Range {
        f32 lo = 0.0f;
        f32 hi = 0.0f;
    };
    /// Level 0 holds one range a cell; level k one per 2^k by 2^k block.
    std::vector<std::vector<Range>> levels_;
    std::vector<u32> widths_;
    std::vector<u32> heights_;
    u32 cells_x_ = 0;
    u32 cells_z_ = 0;

    void visit(u32 level, u32 x, u32 z, const std::vector<Plane>& planes, Box& out,
               bool& any) const;
};

/// What a light camera sees from (M210c): the main camera's view-projection
/// (column-major, Vulkan), its forward axis, its target zoom (the world's
/// extent across the view), and the map's sun (toward the sun).
struct ShadowView {
    std::array<f32, 16> view_proj{};
    std::array<f32, 3> forward{};
    f32 zoom = 0.0f;
    std::array<f32, 3> sun{};
};

/// Moho's Shadow::PrepareLightCamera: the light's view-projection
/// (column-major; Vulkan's depth, y flipped as the main camera's is, so
/// the same culling keeps the same faces), or none past ren_ShadowLOD or
/// with no terrain in view.
/// - The basis: the light travels along L = -sun; the map's up is the
///   camera's forward with its part along L removed, else (0, 0, -1).
/// - The volume: the terrain cells inside the camera's frustum, its far
///   plane ren_ShadowCoeff x zoom past its near plane; its top raised 8.
/// - An orthographic box over the volume's corners seen from 10000 toward
///   the sun, fitted in x and y apart; its near plane 25 nearer.
std::optional<std::array<f32, 16>> shadow_camera(const ShadowView& view,
                                                 const HeightBounds& bounds);

} // namespace osc::renderer
