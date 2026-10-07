#pragma once

// The occupation grid and footprint fitting (roadmap item 4b): Moho's
// COGrid, and OCCUPY_MobileCheck / OCCUPY_FootprintFits (faf-re COGrid.cpp,
// STIMap.cpp). Which cells of the map structures and props stand on, and
// whether a footprint may stand at a place -- the test the pathfinder asks
// of every cell it considers.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"

#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::sim {

/// A rectangle of map cells, [x0, x1) x [z0, z1).
struct OccupancyRect {
    i32 x0 = 0;
    i32 z0 = 0;
    i32 x1 = 0;
    i32 z1 = 0;
};

/// Moho's SFootprint::ToCellPos: the cell a footprint centred at (wx, wz)
/// has its corner in, lrint(world - size / 2).
OccupancyRect footprint_rect(const blueprints::Footprint& fp, f32 wx, f32 wz);

/// Moho's COGrid: one bit a map cell for what stands on the ground (LAND,
/// SEABED or SUB caps) and one for what floats (WATER). Off the map counts
/// as occupied.
class OccupancyGrid {
public:
    OccupancyGrid() = default;
    OccupancyGrid(u32 width, u32 height);

    u32 width() const { return width_; }
    u32 height() const { return height_; }

    /// Mark the rect occupied in the ground grid (caps with LAND, SEABED or
    /// SUB) and/or the water grid (WATER), as Moho's ExecuteOccupy.
    void fill(u8 caps, const OccupancyRect& r, bool occupied);
    /// Whether any cell of the rect is occupied (or off the map).
    bool ground_any(const OccupancyRect& r) const { return any(ground_, r); }
    bool water_any(const OccupancyRect& r) const { return any(water_, r); }
    bool ground_at(i32 x, i32 z) const { return ground_any({x, z, x + 1, z + 1}); }
    bool water_at(i32 x, i32 z) const { return water_any({x, z, x + 1, z + 1}); }
    /// Moho's mOccupation: places units have claimed to come to rest (an
    /// aircraft's landing place, Unit::ReserveOgridRect), which a move's
    /// destination (PrepareMove) keeps clear of. Off the map, nothing.
    void reserve(const OccupancyRect& r, bool reserved) { set(reserved_, r, reserved); }
    bool reserved_any(const OccupancyRect& r) const;

private:
    bool any(const std::vector<u8>& cells, const OccupancyRect& r) const;
    void set(std::vector<u8>& cells, const OccupancyRect& r, bool occupied);

    u32 width_ = 0;
    u32 height_ = 0;
    std::vector<u8> ground_;
    std::vector<u8> water_;
    std::vector<u8> reserved_;
};

/// The caps a footprint at cell `x0, z0` has from the map alone: Moho's
/// OCCUPY_MobileCheck (and OccupancyCapsOfFootprintAt for a single cell).
/// None off the map or on blocking terrain; else its own caps less WATER,
/// SUB and SEABED where the water over its highest point is shallower than
/// MinWaterDepth, and less LAND and SEABED where the water over its lowest
/// is deeper than MaxWaterDepth, or the ground under it steps more than
/// MaxSlope between neighbouring points.
u8 map_caps(const blueprints::Footprint& fp, const map::Terrain& terrain, i32 x0, i32 z0);

/// Moho's OCCUPY_FootprintFits: the caps `fp` has at cell `x0, z0` with
/// what stands there -- `caps` (map_caps when kAnyCaps) less LAND and
/// SEABED on occupied ground (unless it ignores structures) and less WATER
/// on occupied water.
constexpr u8 kAnyCaps = 0xFF;
u8 footprint_fits(const blueprints::Footprint& fp, const map::Terrain& terrain,
                  const OccupancyGrid& grid, i32 x0, i32 z0, u8 caps = kAnyCaps);

/// What one entity occupies: its caps and rects (a footprint, or a
/// blueprint's Physics.OccupyRects).
struct GroundOccupant {
    u8 caps = 0;
    std::vector<OccupancyRect> rects;
};

} // namespace osc::sim
