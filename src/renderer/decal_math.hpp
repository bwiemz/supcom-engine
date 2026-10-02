#pragma once

#include "core/types.hpp"
#include "map/terrain.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace osc::renderer {

/// The technique a decal draws with. Normals (TDecalsNormals and
/// TDecalsNormalsAlpha, alike) draw into the normal pass's target (M212e);
/// the rest over the terrain's colour, in the order HighFidelityTerrain::
/// DrawNormals draws them: the glow masks (TDecalGlowMask), Albedo (TDecals),
/// AlbedoXP (TDecalsXP), then, after the splats, the glowing ones
/// (TDecalsGlow). WaterAlbedo (TDecalsWaterAlbedo, M212g) lies on the
/// water's surface, drawn after the water.
enum class DecalTechnique : u8 { Normals, GlowMask, Albedo, AlbedoXP, Glow, WaterAlbedo };

/// A decal type's technique; none for Water Mask and Water Normals, which
/// Moho never draws.
std::optional<DecalTechnique> decal_technique(map::DecalType type);

/// Moho's CAnimTexture (M212g): a decal's texture whose name ends in a run
/// of digits just before its extension ("foam_01.dds") is the first frame of
/// a sequence, the run counted up ("foam_02.dds", with its carries, as wide
/// as it was) while the next file exists. Every other name is one frame.
/// The next frame's name, in place; false when the name has no such run.
bool next_decal_frame_name(std::string& name);
/// The frames from `first`: it, then each next name `exists` finds (at most
/// 1000, and never round to the first again).
std::vector<std::string> decal_frame_names(const std::string& first,
                                           const std::function<bool(const std::string&)>& exists);
/// The frame of `count` a decal shows at `ticks` (the sim's ticks and the
/// frame's fraction of the next): half a frame a tick (5 a second), every
/// decal in step (CWldTerrainDecal's rate 5 times 0.1 a tick).
size_t decal_frame_at(f32 ticks, size_t count);

/// A decal's texture matrix (CWldTerrainDecal::Update), as DecalsVS
/// applies it to a world position (a row vector, D3D's mul): its corner taken
/// away, turned by D3DX's RotationY, X and Z, over its scale. `u` and `v` are
/// the columns that give the decal's (x, z): 0 to 1 over its footprint.
void decal_texture_matrix(const map::DecalInfo& d, f32 u[4], f32 v[4]);

/// A decal's bounds on the ground (ProjectDecalBoundsXZ): placed by its
/// corner, it runs along its x axis (sx cos, sx sin) and z axis (-sz sin, sz
/// cos) from its position.
void decal_bounds(const map::DecalInfo& d, f32& min_x, f32& min_z, f32& max_x, f32& max_z);

/// A point of a decal's footprint (CWldTerrainDecal::ComputeCorner): (lx,
/// lz) from 0 to 1 along its x and z axes, from its corner. Returns the
/// world's (x, z).
std::array<f32, 2> decal_corner(const map::DecalInfo& d, f32 lx, f32 lz);

/// Moho's LOD metric (GeomCamera3's viewport.r[1], at lodScale 1): the
/// width the screen spans, in world units, at the point's view depth, for a
/// camera at `eye` with view matrix `view` (column-major) whose half width
/// is `half_width` (the tangent of half its horizontal field of view).
f32 decal_lod_metric(const std::array<f32, 16>& view, const std::array<f32, 3>& eye, f32 half_width,
                     f32 x, f32 y, f32 z);

/// CWldTerrainDecal::GetLODAlpha: whole until ren_DecalFadeFraction (0.75)
/// of its cutoff, then fading to none at it; a near cutoff fades it in
/// instead. A cutoff of 0 doesn't fade.
f32 decal_lod_alpha(f32 cutoff, f32 near_cutoff, f32 distance);


} // namespace osc::renderer
