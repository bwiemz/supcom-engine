#include "sim/occupancy.hpp"

#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace oc = blueprints::occupancy;

namespace {

constexpr u8 kGroundCaps = oc::kLand | oc::kSeabed | oc::kSub; // the ground grid's
constexpr u8 kLandSeabed = oc::kLand | oc::kSeabed;
constexpr u8 kWaterSubSeabed = oc::kWater | oc::kSub | oc::kSeabed;
constexpr f32 kNoWater = -10000.0f; // Moho's water elevation on a dry map

} // namespace

OccupancyRect footprint_rect(const blueprints::Footprint& fp, f32 wx, f32 wz) {
    const auto x0 = static_cast<i32>(std::lrint(wx - static_cast<f32>(fp.size_x) * 0.5f));
    const auto z0 = static_cast<i32>(std::lrint(wz - static_cast<f32>(fp.size_z) * 0.5f));
    return {x0, z0, x0 + fp.size_x, z0 + fp.size_z};
}

OccupancyGrid::OccupancyGrid(u32 width, u32 height)
    : width_(width), height_(height), ground_(static_cast<size_t>(width) * height, 0),
      water_(static_cast<size_t>(width) * height, 0) {}

bool OccupancyGrid::any(const std::vector<u8>& cells, const OccupancyRect& r) const {
    if (r.x0 < 0 || r.z0 < 0 || r.x1 > static_cast<i32>(width_) || r.z1 > static_cast<i32>(height_))
        return true;
    for (i32 z = r.z0; z < r.z1; ++z) {
        const size_t row = static_cast<size_t>(z) * width_;
        for (i32 x = r.x0; x < r.x1; ++x)
            if (cells[row + static_cast<size_t>(x)] != 0) return true;
    }
    return false;
}

void OccupancyGrid::set(std::vector<u8>& cells, const OccupancyRect& r, bool occupied) {
    const i32 x0 = std::max(r.x0, 0);
    const i32 z0 = std::max(r.z0, 0);
    const i32 x1 = std::min(r.x1, static_cast<i32>(width_));
    const i32 z1 = std::min(r.z1, static_cast<i32>(height_));
    for (i32 z = z0; z < z1; ++z) {
        const size_t row = static_cast<size_t>(z) * width_;
        for (i32 x = x0; x < x1; ++x) cells[row + static_cast<size_t>(x)] = occupied ? 1 : 0;
    }
}

void OccupancyGrid::fill(u8 caps, const OccupancyRect& r, bool occupied) {
    if ((caps & kGroundCaps) != 0) set(ground_, r, occupied);
    if ((caps & oc::kWater) != 0) set(water_, r, occupied);
}

u8 map_caps(const blueprints::Footprint& fp, const map::Terrain& terrain, i32 x0, i32 z0) {
    const map::Heightmap& hm = terrain.heightmap();
    const auto width = static_cast<i32>(hm.grid_width()); // vertices
    const auto depth = static_cast<i32>(hm.grid_height());
    const bool single = std::max(fp.size_x, fp.size_z) == 1;
    // A single cell is the cell's four corners (OccupancyCapsOfFootprintAt);
    // a larger footprint all the points from its corner to its far corner.
    const i32 x1 = x0 + (single ? 1 : fp.size_x);
    const i32 z1 = z0 + (single ? 1 : fp.size_z);
    if (x0 < 0 || z0 < 0 || x1 > width - 1 || z1 > depth - 1) return 0;
    const auto height = [&](i32 x, i32 z) {
        return hm.get_height_at_grid(static_cast<u32>(x), static_cast<u32>(z));
    };
    f32 lowest = height(x0, z0);
    f32 highest = lowest;
    if (single) {
        if (terrain.is_blocking_cell(x0, z0)) return 0;
    }
    for (i32 z = z0; z <= z1; ++z) {
        for (i32 x = x0; x <= x1; ++x) {
            const f32 h = height(x, z);
            lowest = std::min(lowest, h);
            highest = std::max(highest, h);
            if (!single && terrain.is_blocking_cell(x, z)) return 0;
        }
    }
    const f32 water = terrain.has_water() ? terrain.water_elevation() : kNoWater;
    u8 caps = fp.caps;
    if (fp.min_water_depth > water - highest) caps &= static_cast<u8>(~kWaterSubSeabed);
    if (water - lowest > fp.max_water_depth) caps &= static_cast<u8>(~kLandSeabed);
    if ((caps & kLandSeabed) != 0 && fp.max_slope != 0.0f) {
        // The largest step between neighbours, along each row, then each
        // column. Heights are raw x 1/128, so these differences are exact.
        f32 step = 0;
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0 + 1; x <= x1; ++x)
                step = std::max(step, std::fabs(height(x, z) - height(x - 1, z)));
        for (i32 x = x0; x <= x1; ++x)
            for (i32 z = z0 + 1; z <= z1; ++z)
                step = std::max(step, std::fabs(height(x, z) - height(x, z - 1)));
        if (step > fp.max_slope) caps &= static_cast<u8>(~kLandSeabed);
    }
    return caps;
}

u8 footprint_fits(const blueprints::Footprint& fp, const map::Terrain& terrain,
                  const OccupancyGrid& grid, i32 x0, i32 z0, u8 caps) {
    if (caps == kAnyCaps) caps = map_caps(fp, terrain, x0, z0);
    // A single cell asks its own cell; a larger footprint its rect.
    const bool single = std::max(fp.size_x, fp.size_z) == 1;
    const OccupancyRect r = single ? OccupancyRect{x0, z0, x0 + 1, z0 + 1}
                                   : OccupancyRect{x0, z0, x0 + fp.size_x, z0 + fp.size_z};
    if ((fp.flags & blueprints::kFootprintIgnoreStructures) == 0 && (caps & kLandSeabed) != 0 &&
        grid.ground_any(r))
        caps &= static_cast<u8>(~kLandSeabed);
    if ((caps & oc::kWater) != 0 && grid.water_any(r)) caps &= static_cast<u8>(~oc::kWater);
    return caps;
}

} // namespace osc::sim
