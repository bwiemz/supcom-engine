#pragma once

// The cartographic view's lookup data: what Moho's Cartographic builds once
// from the map (InitializeTerrain, InitializeTerrainTextures; faf-re
// render/Cartographic.cpp) for the minimap's hypsometric, contoured terrain.
// Pure arithmetic, in Moho's float order, for the renderer to upload and the
// tests to check against.

#include "core/types.hpp"

#include <array>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::renderer {

inline constexpr i32 kCartographicRampTexels = 256;

/// The view's constants for a map (Cartographic::InitializeTerrain).
struct CartographicParams {
    /// The elevation texture's size: half the heightfield's sample span.
    i32 grid_width = 0, grid_height = 0;
    std::array<f32, 4> grid_size_coeff{};    ///< {1/gridW, 1/gridH, 0, 1}
    std::array<f32, 4> terrain_size_coeff{}; ///< {gridW/spanX, gridH/spanZ, 0, 1}
    /// The heights' floor and ceiling (the heightfield's bounding box), in
    /// world units: the ramps' 0 and 1.
    f32 elev_min = 0, elev_max = 0;
    /// The map has water: the ramps keep the water's blues and no contours.
    bool has_bounds = false;
    /// The water's surface (where the ground quad lies), deep water's start,
    /// and the abyss lane, which Moho reads from deep water's start too, so
    /// the second blue never shows; all the floor without water.
    f32 surface = 0, water = 0, deep_lane = 0;
    std::array<u32, 5> palette{}; ///< ARGB: abyss, deep, shallow, low land, high land
    i32 bands = 20;               ///< the contour bands, 1 to 100
};

CartographicParams cartographic_params(const map::Terrain& terrain);

/// Moho's elevation kernel (FUN_007D0F70) at heightfield sample (x, z):
/// the sample one up and left of it, clamped to the field, less `baseline`,
/// times `scale`, through a 3x3 weighting that the shipped code applies to
/// that one sample nine times.
f32 cartographic_height(const map::Terrain& terrain, i32 x, i32 z, f32 baseline, f32 scale);

/// The elevation texture: grid_width x grid_height half floats, row by row,
/// each the kernel at the heightfield sample its texel spans to, 0 at the
/// floor and 1 at the ceiling.
std::vector<u16> cartographic_elevation(const map::Terrain& terrain, const CartographicParams& p);

/// The 256-texel colour ramp (ARGB) the elevation indexes.
std::array<u32, kCartographicRampTexels> hypsometric_ramp(const CartographicParams& p);

/// The 256-texel contour ramp: 0 under the water, and on land each band's
/// shade, 16 to 255, which the edge pass turns into contour lines.
std::array<u8, kCartographicRampTexels> topographic_ramp(const CartographicParams& p);

/// The ground quad's height, as InitializeTerrain uploads it: the water's
/// surface (or the floor) in whole height words, truncated.
i16 cartographic_surface_word(const CartographicParams& p);

/// The view of a cartographic frame, top-down: the world rect (x0, z0) to
/// (x1, z1) fills the target, z0 at its top; heights y_low to y_high take
/// depths 1 to 0 (higher is nearer). Column-major, for Vulkan's clip space.
std::array<f32, 16> cartographic_view_proj(f32 x0, f32 z0, f32 x1, f32 z1, f32 y_low, f32 y_high);

/// BlendPackedColor: each byte from + (to - from) * t, truncated.
u32 blend_packed_color(u32 from, u32 to, f32 t);

/// A float as a half float, rounded half up, denormals kept, too large to
/// infinity: the conversion the D3D9 backend falls back to for
/// D3DXFloat32To16Array.
u16 float_to_half(f32 value);

/// x86's truncating float-to-int conversion (cvttss2si): a NaN or a value
/// out of range is INT32_MIN.
i32 truncate_to_i32(f32 value);

} // namespace osc::renderer
