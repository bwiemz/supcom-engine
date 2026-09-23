#pragma once

// Where may a structure go? The rules behind the engine's AI placement
// queries, brain:CanBuildStructureAt and brain:FindPlaceToBuild (roadmap
// M185). The AI calls FindPlaceToBuild repeatedly while queueing a base,
// issuing each build order before asking for the next spot, so a site one of
// the army's units already has an order for must count as taken -- otherwise
// every call returns the same spot.

#include "core/types.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace osc::sim {

class SimState;

/// What placement needs to know about a structure blueprint.
struct PlacementRules {
    f32 size_x = 1.0f; ///< footprint, world units
    f32 size_z = 1.0f;
    bool on_land = true;   ///< Physics.BuildOnLayerCaps
    bool on_water = false;
    enum class Deposit : u8 { None, Mass, Hydrocarbon } deposit = Deposit::None;
};

/// Blueprint lookup (blueprints live in Lua); unknown ids get defaults.
using PlacementRulesLookup = std::function<PlacementRules(const std::string& bp_id)>;

/// An axis-aligned footprint centered at (x, z), sizes in world units.
struct StructureSite {
    f32 x = 0, z = 0;
    f32 size_x = 1.0f, size_z = 1.0f;

    bool overlaps(const StructureSite& o) const;
};

/// Placement checks for one army at one moment. Collects the army's pending
/// build orders once, so a query over many candidate sites stays cheap.
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

    const SimState& sim_;
    PlacementRulesLookup lookup_;
    std::vector<StructureSite> reserved_; ///< the army's pending build orders
    mutable std::map<std::string, PlacementRules> rules_cache_;
};

} // namespace osc::sim
