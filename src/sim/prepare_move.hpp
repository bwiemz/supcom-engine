#pragma once

// Moho's Unit::PrepareMove (faf-re Unit.cpp 0x0062B780): a destination moved
// to the nearest place the unit fits. A ground unit's move goal goes through
// it (roadmap item 4c-2c), and so does an idle aircraft's landing place
// (item 6).

#include "blueprints/footprint.hpp"
#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3
#include "sim/occupancy.hpp"

namespace osc::map {
class Terrain;
}

namespace osc::sim {

class Unit;

/// The footprint PrepareMove tests for `unit` at `dest`: its own, or a
/// flier's landing one (its caps on the ground, LAND for AIR; square on its
/// larger side; WATER too where the water stands over the ground there, if
/// it is TRANSPORTATION or CANLANDONWATER).
blueprints::Footprint move_footprint(const Unit& unit, const map::Terrain& map,
                                     const Vector3& dest);

/// A rectangle of the world (Moho's gpg::Rect2f) a destination keeps out of.
struct MoveExclusion {
    f32 x0 = 0;
    f32 z0 = 0;
    f32 x1 = 0;
    f32 z1 = 0;
};

/// A destination the unit's footprint won't fit, or that another has
/// reserved, moves to the nearest place it will (nearest the unit), looked
/// for in rings `step` cells apart (twice its larger side for 0), 900 places
/// at most, each inside `bounds` by that side. With `exclude`, a place whose
/// cell touches it is no place (Moho's skirt rect). False, `dest` untouched,
/// if none.
bool prepare_move(const Unit& unit, const map::Terrain& map, const OccupancyGrid& grid,
                  const OccupancyRect& bounds, Vector3& dest, i32 step = 0,
                  const MoveExclusion* exclude = nullptr);

} // namespace osc::sim
