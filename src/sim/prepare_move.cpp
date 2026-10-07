#include "sim/prepare_move.hpp"

#include "map/terrain.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace osc::sim {

namespace oc = blueprints::occupancy;

blueprints::Footprint move_footprint(const Unit& unit, const map::Terrain& map,
                                     const Vector3& dest) {
    blueprints::Footprint fp = unit.footprint();
    if (!unit.can_fly()) return fp;
    fp.caps = static_cast<u8>((fp.caps & ~oc::kAir) | oc::kLand);
    const u8 side = std::max<u8>(std::max(fp.size_x, fp.size_z), 1);
    fp.size_x = side;
    fp.size_z = side;
    if (unit.has_category("CANLANDONWATER") && map.has_water() &&
        map.water_elevation() > map.get_terrain_height(dest.x, dest.z))
        fp.caps = static_cast<u8>(fp.caps | oc::kWater);
    return fp;
}

bool prepare_move(const Unit& unit, const map::Terrain& map, const OccupancyGrid& grid,
                  const OccupancyRect& bounds, Vector3& dest) {
    const blueprints::Footprint fp = move_footprint(unit, map, dest);
    const i32 side = std::max<i32>(std::max(fp.size_x, fp.size_z), 1);
    const f32 hx = static_cast<f32>(fp.size_x) * 0.5f;
    const f32 hz = static_cast<f32>(fp.size_z) * 0.5f;
    // Moho's IsPrepareMoveCandidateValid: inside the bounds by its side; its
    // footprint fits (on the Water layer, SUB aside); no one has reserved it.
    const auto valid = [&](i32 x, i32 z, f32 wx, f32 wz) {
        const auto border = static_cast<f32>(side);
        if (wx - border < static_cast<f32>(bounds.x0) ||
            wz - border < static_cast<f32>(bounds.z0) ||
            wx + border > static_cast<f32>(bounds.x1) || wz + border > static_cast<f32>(bounds.z1))
            return false;
        u8 caps = map_caps(fp, map, x, z);
        if (unit.layer() == "Water") caps = static_cast<u8>(caps & ~oc::kSub);
        if (footprint_fits(fp, map, grid, x, z, caps) == 0) return false;
        return !grid.reserved_any({x, z, x + fp.size_x, z + fp.size_z});
    };
    // The cell the goal will be (ToCellPos, rounded). Moho truncates here,
    // the same cell for its destinations, which are cell centres; the
    // engine's order handlers also give cell edges, where only the rounded
    // one is the goal's.
    const OccupancyRect start = footprint_rect(fp, dest.x, dest.z);
    const i32 sx = start.x0;
    const i32 sz = start.z0;
    if (valid(sx, sz, dest.x, dest.z)) return true;
    const i32 step = side * 2;
    std::vector<Vector3> found;
    int looked = 0;
    for (i32 ring = 1;; ++ring) {
        for (i32 rx = -ring; rx <= ring; ++rx) {
            const i32 rz_step = rx == -ring || rx == ring ? 1 : 2 * ring;
            for (i32 rz = -ring; rz <= ring; rz += rz_step) {
                ++looked;
                const i32 x = static_cast<i16>(sx + rx * step);
                const i32 z = static_cast<i16>(sz + rz * step);
                const f32 wx = static_cast<f32>(x) + hx;
                const f32 wz = static_cast<f32>(z) + hz;
                if (valid(x, z, wx, wz)) found.push_back({wx, dest.y, wz});
            }
        }
        if (!found.empty()) break;
        if (looked >= 900) return false;
    }
    const Vector3 at = unit.position();
    f32 best = std::numeric_limits<f32>::infinity();
    for (const Vector3& p : found) {
        const f32 dx = p.x - at.x;
        const f32 dz = p.z - at.z;
        if (const f32 d = dx * dx + dz * dz; d < best) {
            best = d;
            dest.x = p.x;
            dest.z = p.z;
        }
    }
    return true;
}

} // namespace osc::sim
