#pragma once

// Moho's footprint sweeps (roadmap item 4c-2b, faf-re COGrid.cpp
// GridTraversalLine, CAiPathNavigator.cpp IsFootprintClearAlongCellLine /
// IsCellStepClearForUnit): whether a footprint can slide along a straight
// line between two path cells, the test a navigator puts a shortcut to.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"
#include "sim/occupancy.hpp"
#include "sim/path_search.hpp" // Cell

namespace osc::map {
class Terrain;
}

namespace osc::sim::path {

/// Moho's GridTraversalLine (struct_Line): the cells a segment crosses, in
/// order, one grid step at a time. An axis that runs backwards is walked
/// mirrored, so the walk always counts up.
class GridLine {
public:
    GridLine(i32 step, f32 x_start, f32 z_start, f32 x_end, f32 z_end);
    /// The cell it is on.
    Cell cell() const;
    /// On to the next cell, across whichever edge the segment meets first.
    void advance();
    /// Past the segment's end.
    bool beyond_end() const;

private:
    i32 step_ = 1;
    f32 x0_ = 0, x1_ = 0, z0_ = 0, z1_ = 0, dx_ = 0, dz_ = 0;
    i32 x_mask_ = 0, z_mask_ = 0;
    i32 x_edge_ = 0, z_edge_ = 0;
};

/// The footprint test along a line (Moho's IsFootprintClearAlongCellLine):
/// each cell the line from `from` to `to` crosses, both displaced by the
/// same point inside their cell, must take `fp` (map_caps then
/// footprint_fits, SUB dropped on the Water layer). The first cell is
/// always tested.
bool footprint_clear_along_line(const blueprints::Footprint& fp, bool on_water,
                                const map::Terrain& terrain, const OccupancyGrid& grid, Cell from,
                                Cell to, f32 probe_x, f32 probe_z);

/// Moho's IsCellStepClearForUnit: a straight step walked down its middle; a
/// diagonal one twice, its probe against opposite corners of the cells, so
/// it never cuts a corner.
bool cell_step_clear(const blueprints::Footprint& fp, bool on_water, const map::Terrain& terrain,
                     const OccupancyGrid& grid, Cell from, Cell to);

} // namespace osc::sim::path
