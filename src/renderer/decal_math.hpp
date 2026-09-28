#pragma once

#include "core/types.hpp"
#include "map/terrain.hpp"

#include <array>
#include <optional>

namespace osc::renderer {

/// The technique a decal draws over the terrain's colour with, in the order
/// HighFidelityTerrain::DrawNormals draws them: the glow masks (TDecalGlowMask),
/// Albedo (TDecals), AlbedoXP (TDecalsXP), then, after the splats, the glowing
/// ones (TDecalsGlow).
enum class DecalTechnique : u8 { GlowMask, Albedo, AlbedoXP, Glow };

/// A decal type's technique; none for the types that don't draw over the
/// terrain's colour (normals, water).
std::optional<DecalTechnique> decal_technique(map::DecalType type);

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
/// camera at `eye` with view matrix `view` (column-major).
f32 decal_lod_metric(const std::array<f32, 16>& view, const std::array<f32, 3>& eye, f32 aspect,
                     f32 x, f32 y, f32 z);

/// CWldTerrainDecal::GetLODAlpha: whole until ren_DecalFadeFraction (0.75)
/// of its cutoff, then fading to none at it; a near cutoff fades it in
/// instead. A cutoff of 0 doesn't fade.
f32 decal_lod_alpha(f32 cutoff, f32 near_cutoff, f32 distance);

/// The terrain's normal at (x, z), by central differences of its height a
/// unit either side, as the terrain mesh's vertices take theirs.
std::array<f32, 3> terrain_normal_at(const map::Terrain& terrain, f32 x, f32 z);

} // namespace osc::renderer
