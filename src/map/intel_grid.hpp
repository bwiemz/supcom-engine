#pragma once

// An army's intel grids (M215g): Moho's CIntelGrid and the eight of them its
// CAiReconDBImpl keeps (faf-re sim/CIntelGrid.cpp, ai/CAiReconDBImpl.cpp;
// docs/plans/2026-10-01-m215g-intel-grids-design.md).
//
// A grid counts, per cell, the circles of intel over it: a source adds 1 to
// each cell of its circle when it is painted and takes the 1 away when it is
// rubbed out, so a cell is sensed while its count isn't 0. Circles are flat:
// terrain blocks nothing.

#include "core/types.hpp"

#include <array>
#include <vector>

namespace osc::map {

class IntelGrid {
public:
    IntelGrid() = default;
    /// A grid of `cell_size`-unit cells over a map of `map_width` by
    /// `map_height` units (Moho: the heightfield's size less 1, over the
    /// cell size, rounded down).
    IntelGrid(u32 map_width, u32 map_height, u32 cell_size);

    u32 cell_size() const { return cell_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    bool empty() const { return cells_.empty(); }

    /// Paint, or rub out, a circle of `radius` world units about (x, z)
    /// (AddCircle/SubtractCircle: the radius in whole cells, rounded down).
    void add_circle(f32 x, f32 z, u32 radius) { raster(x, z, radius / cell_, 1); }
    void sub_circle(f32 x, f32 z, u32 radius) { raster(x, z, radius / cell_, -1); }

    /// The cell under (x, z), floored (Moho's GridPos).
    i32 cell_x(f32 x) const;
    i32 cell_z(f32 z) const;
    /// Whether the cell's count isn't 0; off the grid, no.
    bool visible_cell(i32 gx, i32 gz) const;
    /// Whether the cell under (x, z) is sensed (CIntelGrid::IsVisible).
    bool visible(f32 x, f32 z) const { return visible_cell(cell_x(x), cell_z(z)); }
    /// Whether any cell of the world rectangle from (floor x0, floor z0) to
    /// (ceil x1, ceil z1) has a count above 0 (IsVisible of a rect: its
    /// corners in whole units, then cells from the floored least to the
    /// ceiled greatest, exclusive).
    bool any_in(f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// A cell's count (tests and the snapshot's copies).
    i8 count(u32 gx, u32 gz) const { return cells_[static_cast<size_t>(gz) * width_ + gx]; }
    const std::vector<i8>& cells() const { return cells_; }

private:
    /// CIntelGrid::Raster: columns gx-r to gx+r (exclusive), each of rows
    /// gz-leg to gz+leg (exclusive), leg = (int)sqrt(r^2 - dx^2); clipped to
    /// the grid.
    void raster(f32 x, f32 z, u32 radius_cells, i8 delta);

    u32 cell_ = 1;
    u32 width_ = 0;
    u32 height_ = 0;
    std::vector<i8> cells_;
};

/// The grids each army keeps, by Moho's names. Vision and water are line
/// of sight above and under the water's surface; the counter grids hold
/// other armies' stealth and cloak fields.
enum class IntelLayer : u8 {
    Vision, ///< 2-unit cells
    Water,  ///< underwater line of sight (WaterVision), 4-unit cells
    Radar,
    Sonar,
    Omni,
    RadarCounter,  ///< RCI: radar stealth fields
    SonarCounter,  ///< SCI: sonar stealth fields
    VisionCounter, ///< VCI: cloak fields
    Count,
};
constexpr size_t kIntelLayers = static_cast<size_t>(IntelLayer::Count);

/// Moho's cell sizes: 2 for vision, 4 for the rest.
constexpr u32 intel_cell_size(IntelLayer layer) {
    return layer == IntelLayer::Vision ? 2 : 4;
}

/// Every army's grids. Without fog of war there are no vision or water
/// grids (Moho makes none), and every army has line of sight everywhere.
class IntelGrids {
public:
    IntelGrids(u32 map_width, u32 map_height, u32 armies, bool fog_of_war);

    u32 map_width() const { return map_width_; }
    u32 map_height() const { return map_height_; }
    u32 armies() const { return static_cast<u32>(grids_.size()); }
    bool fog_of_war() const { return fog_; }

    /// Army `army`'s grid of `layer`; empty if it keeps none.
    IntelGrid& grid(u32 army, IntelLayer layer) { return grids_[army][static_cast<size_t>(layer)]; }
    const IntelGrid& grid(u32 army, IntelLayer layer) const {
        return grids_[army][static_cast<size_t>(layer)];
    }

private:
    u32 map_width_ = 0;
    u32 map_height_ = 0;
    bool fog_ = true;
    std::vector<std::array<IntelGrid, kIntelLayers>> grids_;
};

} // namespace osc::map
