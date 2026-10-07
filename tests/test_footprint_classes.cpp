// Footprint classes (roadmap item 4): /lua/footprints.lua's specs through
// SpecFootprints, and the class each unit blueprint resolves to, as Moho's
// RRuleGameRules::FindFootprint and RUnitBlueprintPhysics::
// ComputeDerivedQuantities (faf-re) pick them.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"

#include <limits>
#include <string>
#include <vector>

using osc::blueprints::Footprint;
using osc::blueprints::NamedFootprint;
using osc::blueprints::UnitFootprints;
namespace oc = osc::blueprints::occupancy;

namespace {

// Retail's /lua/footprints.lua (mohodata.scd): it defines the caps and
// flag it is written with, then specs the classes. FAF's is the same but
// for WaterLand4x4 where retail has WaterLand5x5.
const char* const kRetailSpec = R"(
LAND = 0x01
SEABED = 0x02
SUB = 0x04
WATER = 0x08
AIR = 0x10
ORBIT = 0x20
IgnoreStructures = 0x01
SpecFootprints {
    { Name = 'Vehicle1x1',   SizeX=1,  SizeZ=1,  Caps=LAND, MaxWaterDepth=0.05, MaxSlope=0.75, Flags=0 },
    { Name = 'Vehicle2x2',   SizeX=2,  SizeZ=2,  Caps=LAND, MaxWaterDepth=0.05, MaxSlope=0.75, Flags=0 },
    { Name = 'Vehicle5x5',   SizeX=5,  SizeZ=5,  Caps=LAND, MaxWaterDepth=0.05, MaxSlope=0.75, Flags=IgnoreStructures },
    { Name = 'Amphibious1x1',   SizeX=1,  SizeZ=1,  Caps=LAND|SEABED, MaxWaterDepth=25, MaxSlope=0.75, Flags=0 },
    { Name = 'Amphibious3x3',   SizeX=3,  SizeZ=3,  Caps=LAND|SEABED, MaxWaterDepth=25, MaxSlope=0.75, Flags=IgnoreStructures },
    { Name = 'Amphibious6x6',   SizeX=6,  SizeZ=6,  Caps=LAND|SEABED, MaxWaterDepth=25, MaxSlope=0.75, Flags=IgnoreStructures },
    { Name = 'WaterLand1x1',   SizeX=1,  SizeZ=1,  Caps=LAND|WATER, MaxWaterDepth=1, MinWaterDepth=0.1, MaxSlope=0.75, Flags=0 },
    { Name = 'WaterLand2x2',   SizeX=2,  SizeZ=2,  Caps=LAND|WATER, MaxWaterDepth=1, MinWaterDepth=0.1, MaxSlope=0.75, Flags=0 },
    { Name = 'WaterLand3x3',   SizeX=3,  SizeZ=3,  Caps=LAND|WATER, MaxWaterDepth=5, MinWaterDepth=0, MaxSlope=0.75, Flags=0 },
    { Name = 'WaterLand5x5',   SizeX=5,  SizeZ=5,  Caps=LAND|WATER, MaxWaterDepth=5, MinWaterDepth=0, MaxSlope=0.75, Flags=0 },
    { Name = 'SurfacingSub2x2',   SizeX=2,  SizeZ=2,  Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'SurfacingSub3x3',   SizeX=3,  SizeZ=3,  Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'SurfacingSub4x4',   SizeX=4,  SizeZ=4,  Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'SurfacingSub12x12', SizeX=12, SizeZ=12, Caps=SUB|WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water1x1',   SizeX=1,  SizeZ=1,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water3x3',   SizeX=3,  SizeZ=3,  Caps=WATER, MinWaterDepth=0.25, Flags=0 },
    { Name = 'Water4x4',   SizeX=4,  SizeZ=4,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water6x6',   SizeX=6,  SizeZ=6,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water8x8',   SizeX=8,  SizeZ=8,  Caps=WATER, MinWaterDepth=1.5, Flags=0 },
    { Name = 'Water11x11', SizeX=11, SizeZ=11, Caps=WATER, MinWaterDepth=1.5, Flags=0 },
}
)";

/// The retail classes, parsed through SpecFootprints as the loader does.
struct RetailClasses {
    osc::lua::LuaState lua;
    osc::blueprints::BlueprintStore store{lua.raw()};
    RetailClasses() {
        lua.set_blueprint_store(&store);
        osc::lua::register_blueprint_store_bindings(lua);
        REQUIRE(lua.do_string(kRetailSpec).ok());
    }
    const std::vector<NamedFootprint>& classes() const { return store.footprint_classes(); }
};

Footprint sized(int x, int z) {
    Footprint fp;
    fp.size_x = static_cast<osc::u8>(x);
    fp.size_z = static_cast<osc::u8>(z);
    return fp;
}

std::string class_of(const RetailClasses& r, const char* motion, int x, int z) {
    const UnitFootprints f =
        osc::blueprints::resolve_unit_footprints(r.classes(), sized(x, z), motion, "", 0);
    return f.main_class < 0 ? "-" : r.classes()[static_cast<size_t>(f.main_class)].name;
}

} // namespace

TEST_CASE("SpecFootprints keeps retail's classes in spec order", "[footprints]") {
    RetailClasses r;
    REQUIRE(r.classes().size() == 20);
    const NamedFootprint& amphibious = r.classes()[4];
    CHECK(amphibious.name == "Amphibious3x3");
    CHECK(amphibious.index == 4);
    CHECK(amphibious.caps == (oc::kLand | oc::kSeabed)); // LuaPlus's `|`
    CHECK(amphibious.flags == osc::blueprints::kFootprintIgnoreStructures);
    CHECK(amphibious.max_water_depth == 25.0f);
    CHECK(amphibious.max_slope == 0.75f);
    CHECK(r.classes()[7].min_water_depth == 0.1f);
    CHECK(r.classes()[19].name == "Water11x11");
}

TEST_CASE("A unit paths as the class of its motion's caps nearest its size", "[footprints]") {
    RetailClasses r;
    CHECK(class_of(r, "RULEUMT_Land", 1, 1) == "Vehicle1x1");
    CHECK(class_of(r, "RULEUMT_Biped", 2, 2) == "Vehicle2x2");
    CHECK(class_of(r, "RULEUMT_Land", 3, 3) == "Vehicle2x2"); // 1 off, not 2
    CHECK(class_of(r, "RULEUMT_Land", 4, 4) == "Vehicle5x5");
    CHECK(class_of(r, "RULEUMT_Land", 9, 9) == "Vehicle5x5");
    CHECK(class_of(r, "RULEUMT_Land", 1, 4) == "Vehicle2x2"); // the larger of the two offs
    CHECK(class_of(r, "RULEUMT_Amphibious", 6, 6) == "Amphibious6x6");
    CHECK(class_of(r, "RULEUMT_Hover", 2, 2) == "WaterLand2x2");
    CHECK(class_of(r, "RULEUMT_AmphibiousFloating", 3, 3) == "WaterLand3x3");
    CHECK(class_of(r, "RULEUMT_SurfacingSub", 12, 12) == "SurfacingSub12x12");
    CHECK(class_of(r, "RULEUMT_Water", 2, 2) == "Water1x1"); // 1 off either way: the first
    CHECK(class_of(r, "RULEUMT_Water", 5, 5) == "Water4x4");
    CHECK(class_of(r, "RULEUMT_Hover", 4, 4) == "WaterLand3x3");
    // Fliers have no ground class.
    CHECK(class_of(r, "RULEUMT_Air", 2, 2) == "-");
    CHECK(class_of(r, "RULEUMT_Special", 2, 2) == "-");
}

TEST_CASE("The class replaces a mobile unit's footprint whole", "[footprints]") {
    RetailClasses r;
    Footprint own = sized(4, 4);
    own.max_slope = 0.2f;
    const UnitFootprints f =
        osc::blueprints::resolve_unit_footprints(r.classes(), own, "RULEUMT_Land", "", 0);
    CHECK(f.main.size_x == 5);
    CHECK(f.main.size_z == 5);
    CHECK(f.main.caps == oc::kLand);
    CHECK(f.main.flags == osc::blueprints::kFootprintIgnoreStructures);
    CHECK(f.main.max_slope == 0.75f);
    CHECK(f.main.max_water_depth == 0.05f);
    // No AltMotionType: the alt is the main.
    CHECK(f.alt == f.main);
    CHECK(f.alt_class == f.main_class);

    // A flier keeps its own size, with AIR caps and no class.
    const UnitFootprints air =
        osc::blueprints::resolve_unit_footprints(r.classes(), own, "RULEUMT_Air", "", 0);
    CHECK(air.main_class == -1);
    CHECK(air.main.caps == oc::kAir);
    CHECK(air.main.size_x == 4);
}

TEST_CASE("An AltMotionType has a footprint of its own, else the main one", "[footprints]") {
    RetailClasses r;
    // A land unit that also floats (AltMotionType Hover).
    const UnitFootprints f = osc::blueprints::resolve_unit_footprints(
        r.classes(), sized(2, 2), "RULEUMT_Land", "RULEUMT_Hover", 0);
    CHECK(r.classes()[static_cast<size_t>(f.main_class)].name == "Vehicle2x2");
    CHECK(r.classes()[static_cast<size_t>(f.alt_class)].name == "WaterLand2x2");
    // One whose alt caps have no class falls back to the main footprint.
    const UnitFootprints g = osc::blueprints::resolve_unit_footprints(
        r.classes(), sized(2, 2), "RULEUMT_Land", "RULEUMT_Special", 0);
    CHECK(g.alt == g.main);
    CHECK(g.alt_class == g.main_class);
}

TEST_CASE("A structure stands where it may be built", "[footprints]") {
    RetailClasses r;
    Footprint own = sized(3, 3);
    own.min_water_depth = 1.5f;
    const UnitFootprints dock = osc::blueprints::resolve_unit_footprints(
        r.classes(), own, "RULEUMT_None", "", oc::kWater | oc::kSub);
    CHECK(dock.main_class == -1); // structures have no path class
    CHECK(dock.main.caps == (oc::kWater | oc::kSub));
    CHECK(dock.main.size_x == 3);
    CHECK(dock.main.min_water_depth == 1.5f);
    // One that may stand on the seabed, with no MaxWaterDepth: any depth.
    const UnitFootprints extractor = osc::blueprints::resolve_unit_footprints(
        r.classes(), sized(2, 2), "", "", oc::kLand | oc::kSeabed);
    CHECK(extractor.main.max_water_depth == std::numeric_limits<osc::f32>::max());
}

TEST_CASE("A reload's footprints.lua specs the classes anew", "[footprints]") {
    RetailClasses r;
    REQUIRE(r.classes().size() == 20);
    r.store.rebind(r.lua.raw());
    CHECK(r.classes().empty());
    REQUIRE(r.lua.do_string(kRetailSpec).ok());
    CHECK(r.classes().size() == 20);
    CHECK(r.classes()[19].index == 19);
}
