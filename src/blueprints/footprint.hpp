#pragma once

// Footprint classes (roadmap item 4, faf-re SFootprint / SNamedFootprint /
// RRuleGameRules::FindFootprint / RUnitBlueprintPhysics::
// ComputeDerivedQuantities): the shapes the pathfinder knows, which
// /lua/footprints.lua specs through SpecFootprints, and the one each unit
// blueprint resolves to.

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace osc::blueprints {

/// What a footprint may stand on (Moho's EOccupancyCaps; the same bits as
/// its ELayer).
namespace occupancy {
constexpr u8 kLand = 0x01;
constexpr u8 kSeabed = 0x02;
constexpr u8 kSub = 0x04;
constexpr u8 kWater = 0x08;
constexpr u8 kAir = 0x10;
constexpr u8 kOrbit = 0x20;
/// The caps a pathfinder footprint is resolved for (Moho's 0x0F).
constexpr u8 kGround = kLand | kSeabed | kSub | kWater;
} // namespace occupancy

/// FPFLAG_IgnoreStructures: it paths through structures (Moho's big
/// vehicles and amphibians crush their way).
constexpr u8 kFootprintIgnoreStructures = 0x01;

/// Moho's SFootprint: a rectangle of whole cells and where it may stand.
struct Footprint {
    u8 size_x = 0;
    u8 size_z = 0;
    u8 caps = 0;  ///< occupancy::*
    u8 flags = 0; ///< kFootprintIgnoreStructures
    f32 max_slope = 0;
    f32 min_water_depth = 0;
    f32 max_water_depth = 0;

    bool operator==(const Footprint& o) const {
        return size_x == o.size_x && size_z == o.size_z && caps == o.caps && flags == o.flags &&
               max_slope == o.max_slope && min_water_depth == o.min_water_depth &&
               max_water_depth == o.max_water_depth;
    }
};

/// One class of /lua/footprints.lua (Moho's SNamedFootprint): its index is
/// its place in the spec, which is also its path table's.
struct NamedFootprint : Footprint {
    std::string name;
    i32 index = -1;
};

/// The caps a unit of MotionType `motion` moves with (Moho's table, by
/// RULEUMT_*): Land and Biped LAND, Air AIR, Water WATER, SurfacingSub
/// SUB|WATER, Amphibious LAND|SEABED, Hover and AmphibiousFloating
/// LAND|WATER, None and Special nothing.
u8 motion_type_caps(std::string_view motion);

/// Moho's FindFootprint: the class with exactly `fp`'s caps whose size is
/// nearest (the larger of |dX| and |dZ|; the first in the spec on a tie),
/// or null with none of those caps.
const NamedFootprint* find_footprint(const std::vector<NamedFootprint>& classes,
                                     const Footprint& fp);

/// A unit blueprint's footprints, as Moho resolves them.
struct UnitFootprints {
    Footprint main;
    Footprint alt;       ///< for its AltMotionType (its main one without)
    i32 main_class = -1; ///< the classes they are (-1: none)
    i32 alt_class = -1;
};

/// Resolve a unit blueprint's footprints (Moho's RUnitBlueprintPhysics::
/// ComputeDerivedQuantities). `own` is the blueprint's Footprint table (its
/// size already rounded up from SizeX/SizeZ where it gives none), `motion`
/// and `alt_motion` its Physics.MotionType and AltMotionType,
/// `build_on_layer_caps` its BuildOnLayerCaps as layer bits.
/// - A mobile unit (MotionType not None) takes its caps from the motion
///   type; with any ground cap, the nearest class replaces its footprint
///   whole. Its alt footprint is resolved the same way from AltMotionType,
///   falling back to the main one.
/// - A structure (None) takes its caps from where it may be built; one that
///   may stand on the seabed with no MaxWaterDepth has no depth limit.
UnitFootprints resolve_unit_footprints(const std::vector<NamedFootprint>& classes,
                                       const Footprint& own, std::string_view motion,
                                       std::string_view alt_motion, u8 build_on_layer_caps);

} // namespace osc::blueprints
