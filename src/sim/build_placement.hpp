#pragma once

// Where may a structure go? The rules behind the engine's AI placement
// queries, brain:CanBuildStructureAt and brain:FindPlaceToBuild (roadmap
// M185). The AI calls FindPlaceToBuild repeatedly while queueing a base,
// issuing each build order before asking for the next spot, so a site one of
// the army's units already has an order for must count as taken -- otherwise
// every call returns the same spot.

#include "core/types.hpp"

#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace osc::sim {

class SimState;

/// What placement needs to know about a structure blueprint.
struct PlacementRules {
    f32 size_x = 1.0f; ///< footprint, world units
    f32 size_z = 1.0f;
    /// Physics.SkirtSizeX/Z and SkirtOffsetX/Z: the pad no other structure's
    /// may overlap (none: the footprint)
    f32 skirt_x = 0.0f;
    f32 skirt_z = 0.0f;
    f32 skirt_off_x = 0.0f;
    f32 skirt_off_z = 0.0f;
    bool on_land = true;   ///< Physics.BuildOnLayerCaps
    bool on_water = false;
    bool on_seabed = false; ///< it sits on the ground under water (an extractor)
    enum class Deposit : u8 { None, Mass, Hydrocarbon } deposit = Deposit::None;
};

/// Snap a structure's center to the build grid, as Moho does: the
/// footprint's corner to the nearest whole cell, so odd footprints center on
/// a cell and even ones on a cell corner. The placement ghost and the build
/// order both use it.
inline void snap_structure_center(f32& x, f32& z, f32 size_x, f32 size_z) {
    x = std::nearbyint(x - size_x * 0.5f) + size_x * 0.5f;
    z = std::nearbyint(z - size_z * 0.5f) + size_z * 0.5f;
}

/// The sites of a structure laid along a drag from (x0, z0) to (x1, z1), as
/// Moho lays a build drag: `spacing` apart along the drag's longer axis, the
/// other following it, each snapped as snap_structure_center does; the line
/// stops short of a spacing that doesn't fit. One site for no drag.
std::vector<std::pair<f32, f32>> structure_line_sites(f32 x0, f32 z0, f32 x1, f32 z1, f32 size_x,
                                                      f32 size_z, f32 spacing);

/// Blueprint lookup (blueprints live in Lua); unknown ids get defaults.
using PlacementRulesLookup = std::function<PlacementRules(const std::string& bp_id)>;

/// The pad a structure takes, an axis-aligned rect in world units.
struct StructureSite {
    f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;

    /// The skirt of a footprint `size_x` by `size_z` centred at (x, z)
    static StructureSite of(f32 x, f32 z, f32 size_x, f32 size_z, f32 skirt_x = 0, f32 skirt_z = 0,
                            f32 off_x = 0, f32 off_z = 0);
    static StructureSite of(const PlacementRules& r, f32 x, f32 z);

    bool overlaps(const StructureSite& o) const;
    /// Moho's build-mode adjacency: an edge within 1 of the other's, and the
    /// span along it inside the other's or holding it
    bool touches(const StructureSite& o) const;
};

/// Placement checks for one army at one moment. Collects the army's pending
/// build orders once, the first time a site passes the terrain checks, so a
/// query over many candidate sites stays cheap (and one the terrain rules
/// out doesn't walk the units at all).
class StructurePlacement {
public:
    StructurePlacement(const SimState& sim, i32 army, PlacementRulesLookup rules);

    /// Can `bp_id` be built centered at (x, z)? Requires the footprint inside
    /// the playable area, over cells of a layer the blueprint builds on, clear
    /// of every structure (finished or under construction) and of the army's
    /// pending build orders, and -- for extractors -- over a deposit.
    bool can_build(const std::string& bp_id, f32 x, f32 z) const;

    /// The rules for `bp_id` (cached per query object).
    const PlacementRules& rules(const std::string& bp_id) const;

private:
    bool terrain_allows(const PlacementRules& r, const StructureSite& site) const;
    bool structure_overlaps(const StructureSite& site) const;
    bool on_deposit(const PlacementRules& r, f32 x, f32 z) const;

    /// The army's pending build orders.
    const std::vector<StructureSite>& reserved() const;

    const SimState& sim_;
    i32 army_;
    PlacementRulesLookup lookup_;
    mutable std::optional<std::vector<StructureSite>> reserved_; ///< see reserved()
    mutable std::map<std::string, PlacementRules> rules_cache_;
};

} // namespace osc::sim
