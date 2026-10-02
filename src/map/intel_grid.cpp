#include "map/intel_grid.hpp"

#include <algorithm>
#include <cmath>

namespace osc::map {

IntelGrid::IntelGrid(u32 map_width, u32 map_height, u32 cell_size)
    : cell_(std::max<u32>(cell_size, 1)), width_(map_width / cell_), height_(map_height / cell_),
      cells_(static_cast<size_t>(width_) * height_, 0) {}

i32 IntelGrid::cell_x(f32 x) const {
    return static_cast<i32>(std::floor(x * (1.0f / static_cast<f32>(cell_))));
}

i32 IntelGrid::cell_z(f32 z) const {
    return static_cast<i32>(std::floor(z * (1.0f / static_cast<f32>(cell_))));
}

bool IntelGrid::visible_cell(i32 gx, i32 gz) const {
    if (gx < 0 || gz < 0 || static_cast<u32>(gx) >= width_ || static_cast<u32>(gz) >= height_)
        return false;
    return count(static_cast<u32>(gx), static_cast<u32>(gz)) != 0;
}

bool IntelGrid::any_in(f32 x0, f32 z0, f32 x1, f32 z1) const {
    if (cells_.empty()) return false;
    const auto g = static_cast<f32>(cell_);
    // The rect in whole units (floored least corner, ceiled greatest), then
    // in cells (FloorDivToGridCell, CeilDivToGridCell).
    const auto lo = [&](f32 w) { return static_cast<i64>(std::floor(std::floor(w) / g)); };
    const auto hi = [&](f32 w) { return static_cast<i64>(std::ceil(std::ceil(w) / g)); };
    const i64 gx0 = std::max<i64>(0, lo(x0));
    const i64 gz0 = std::max<i64>(0, lo(z0));
    const i64 gx1 = std::min<i64>(width_, hi(x1));
    const i64 gz1 = std::min<i64>(height_, hi(z1));
    for (i64 z = gz0; z < gz1; ++z)
        for (i64 x = gx0; x < gx1; ++x)
            // Strictly above 0 here, where a point asks for "not 0"
            // (Moho's own asymmetry).
            if (count(static_cast<u32>(x), static_cast<u32>(z)) > 0) return true;
    return false;
}

void IntelGrid::raster(f32 x, f32 z, u32 radius_cells, i8 delta) {
    if (cells_.empty()) return;
    const i32 gx = cell_x(x);
    const i32 gz = cell_z(z);
    const auto width = static_cast<i32>(width_);
    const auto height = static_cast<i32>(height_);
    const auto r = static_cast<i32>(radius_cells);

    const i32 x_begin = std::clamp(gx - r, 0, width);
    const i32 x_end = std::clamp(gx + r, 0, width);
    const i32 r2 = r * r;
    for (i32 cx = x_begin; cx < x_end; ++cx) {
        const i32 dx = gx - cx;
        const auto leg = static_cast<i32>(std::sqrt(static_cast<f32>(r2 - dx * dx)));
        const i32 z_begin = std::clamp(gz - leg, 0, height);
        const i32 z_end = std::clamp(gz + leg, 0, height);
        for (i32 cz = z_begin; cz < z_end; ++cz) {
            i8& cell = cells_[static_cast<size_t>(cz) * width_ + static_cast<size_t>(cx)];
            cell = static_cast<i8>(cell + delta);
        }
    }
}

IntelGrids::IntelGrids(u32 map_width, u32 map_height, u32 armies, bool fog_of_war)
    : map_width_(map_width), map_height_(map_height), fog_(fog_of_war), grids_(armies) {
    for (auto& army : grids_) {
        for (size_t i = 0; i < kIntelLayers; ++i) {
            const auto layer = static_cast<IntelLayer>(i);
            if (!fog_of_war && (layer == IntelLayer::Vision || layer == IntelLayer::Water))
                continue;
            army[i] = IntelGrid(map_width, map_height, intel_cell_size(layer));
        }
    }
}

} // namespace osc::map
