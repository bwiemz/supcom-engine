#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::renderer {

/// A map's normal maps as one texture (M212e), one texel a world unit,
/// which the normal pass's basis samples: the map's own tiles, side by side
/// as Moho lays them (IWldTerrainRes::GetNormalMapInfo: tile i at
/// ((i mod perRow)·w, (i div perRow)·h), perRow = the map's width / w), or,
/// for a terrain without any, one made from its heights. Either way x is in
/// alpha and z in green, y up.
struct TerrainNormalMaps {
    std::vector<char> dds; ///< the tiles as one DDS (DXT5), when the map has them
    std::vector<u8> rgba;  ///< else one made from the heights, width x height
    u32 width = 0, height = 0;
    u32 tile_width = 0, tile_height = 0; ///< a tile's texels (the whole, when made)
};

TerrainNormalMaps terrain_normal_maps(const map::Terrain& terrain);

/// Tiles of `tile_w` x `tile_h` DXT5 texels, `per_row` a row, as one DDS
/// (their mip 0): their blocks side by side. Empty if any isn't a DXT5 DDS
/// of that size.
std::vector<char> combine_dxt5_tiles(const std::vector<std::vector<char>>& tiles, u32 tile_w,
                                     u32 tile_h, u32 per_row);

} // namespace osc::renderer
