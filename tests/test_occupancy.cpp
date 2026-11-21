// The occupation grid and footprint fitting (roadmap item 4b): Moho's
// OCCUPY_MobileCheck / OCCUPY_FootprintFits and COGrid (faf-re STIMap.cpp,
// COGrid.cpp), and the claims structures and props make on it.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/occupancy.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::u8;
using osc::blueprints::Footprint;
using osc::sim::EntityId;
using osc::sim::GroundOccupant;
using osc::sim::OccupancyGrid;
using osc::sim::OccupancyRect;
namespace oc = osc::blueprints::occupancy;

namespace {

constexpr f32 kScale = 1.0f / 128.0f; // a height unit, as the .scmap's

/// A 64 x 64 map: land at height 10 east of x = 32, a seabed at 0 west of
/// it, a cliff (height 20) from z = 48, water at 5 when `wet`.
std::unique_ptr<osc::map::Terrain> terrain(bool wet) {
    std::vector<osc::u16> heights(65 * 65);
    for (int z = 0; z <= 64; ++z)
        for (int x = 0; x <= 64; ++x)
            heights[static_cast<size_t>(z) * 65 + static_cast<size_t>(x)] =
                static_cast<osc::u16>((z >= 48  ? 20.0f
                                       : x > 32 ? 10.0f
                                                : 0.0f) /
                                      kScale);
    osc::map::Heightmap hm(64, 64, kScale, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), 5.0f, wet);
}

constexpr Footprint fp(int size, u8 caps, f32 max_depth, f32 min_depth, f32 slope) {
    Footprint f;
    f.size_x = static_cast<u8>(size);
    f.size_z = static_cast<u8>(size);
    f.caps = caps;
    f.max_water_depth = max_depth;
    f.min_water_depth = min_depth;
    f.max_slope = slope;
    return f;
}

// Retail's classes, as footprints.lua specs them.
constexpr Footprint kVehicle2x2 = fp(2, oc::kLand, 0.05f, 0, 0.75f);
constexpr Footprint kAmphibious3x3 = fp(3, oc::kLand | oc::kSeabed, 25, 0, 0.75f);
constexpr Footprint kWaterLand1x1 = fp(1, oc::kLand | oc::kWater, 1, 0.1f, 0.75f);
constexpr Footprint kWater3x3 = fp(3, oc::kWater, 0, 0.25f, 0);

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

} // namespace

TEST_CASE("A footprint's caps on the map: depths, slope, blocking ground and edges",
          "[occupancy]") {
    const auto wet = terrain(true);
    using osc::sim::map_caps;
    // On land, a vehicle stands; in the water at 5 deep, it doesn't.
    CHECK(map_caps(kVehicle2x2, *wet, 40, 10) == oc::kLand);
    CHECK(map_caps(kVehicle2x2, *wet, 10, 10) == 0);
    // An amphibian: on land LAND alone (no water over it, so no seabed);
    // under 5 of water both, within its MaxWaterDepth of 25.
    CHECK(map_caps(kAmphibious3x3, *wet, 40, 10) == oc::kLand);
    CHECK(map_caps(kAmphibious3x3, *wet, 10, 10) == (oc::kLand | oc::kSeabed));
    // A ship needs its water: on land none.
    CHECK(map_caps(kWater3x3, *wet, 10, 10) == oc::kWater);
    CHECK(map_caps(kWater3x3, *wet, 40, 10) == 0);
    // A hovercraft: 5 deep is past its MaxWaterDepth of 1 for LAND, but it
    // floats.
    CHECK(map_caps(kWaterLand1x1, *wet, 10, 10) == oc::kWater);
    CHECK(map_caps(kWaterLand1x1, *wet, 40, 10) == oc::kLand);
    // Across the shore (x = 32, a step of 10): too steep for the vehicle.
    CHECK(map_caps(kVehicle2x2, *wet, 31, 10) == 0);
    // Up the cliff at z = 48, the same.
    CHECK(map_caps(kVehicle2x2, *wet, 40, 46) == 0);
    CHECK(map_caps(kVehicle2x2, *wet, 40, 50) == oc::kLand);
    // The map's last row and column, and past them: nothing stands there.
    CHECK(map_caps(kVehicle2x2, *wet, 62, 10) == 0);
    CHECK(map_caps(kVehicle2x2, *wet, 61, 10) == oc::kLand);
    CHECK(map_caps(kVehicle2x2, *wet, -1, 10) == 0);

    // On a dry map there is no water, so the seabed is land.
    const auto dry = terrain(false);
    CHECK(map_caps(kVehicle2x2, *dry, 10, 10) == oc::kLand);
    CHECK(map_caps(kWater3x3, *dry, 10, 10) == 0);
}

TEST_CASE("Blocking terrain types take every cap", "[occupancy]") {
    auto t = terrain(true);
    std::vector<u8> types(64 * 64, 1);
    types[10 * 64 + 41] = 7; // one blocking cell under (40, 10)'s footprint
    t->set_terrain_types(std::move(types));
    std::array<bool, 256> blocking{};
    blocking[7] = true;
    t->set_blocking_types(blocking);
    CHECK(osc::sim::map_caps(kVehicle2x2, *t, 40, 10) == 0);
    CHECK(osc::sim::map_caps(kVehicle2x2, *t, 44, 10) == oc::kLand);
    // A single cell asks only its own.
    const Footprint one = fp(1, oc::kLand, 0.05f, 0, 0.75f);
    CHECK(osc::sim::map_caps(one, *t, 41, 10) == 0);
    CHECK(osc::sim::map_caps(one, *t, 40, 10) == oc::kLand);
}

TEST_CASE("Occupied ground and water take their caps, unless structures are ignored",
          "[occupancy]") {
    const auto t = terrain(true);
    OccupancyGrid grid(64, 64);
    using osc::sim::footprint_fits;
    CHECK(footprint_fits(kVehicle2x2, *t, grid, 40, 10) == oc::kLand);
    grid.fill(oc::kLand, {41, 11, 42, 12}, true);
    CHECK(footprint_fits(kVehicle2x2, *t, grid, 40, 10) == 0);
    CHECK(footprint_fits(kVehicle2x2, *t, grid, 42, 12) == oc::kLand);
    // Vehicle5x5 and the big amphibians path through structures.
    Footprint crusher = kAmphibious3x3;
    crusher.flags = osc::blueprints::kFootprintIgnoreStructures;
    CHECK(footprint_fits(crusher, *t, grid, 40, 10) == oc::kLand);
    // A floating structure takes the water only.
    grid.fill(oc::kWater, {10, 10, 13, 13}, true);
    CHECK(footprint_fits(kWater3x3, *t, grid, 10, 10) == 0);
    CHECK(footprint_fits(kAmphibious3x3, *t, grid, 10, 10) == (oc::kLand | oc::kSeabed));
    grid.fill(oc::kLand, {41, 11, 42, 12}, false);
    CHECK(footprint_fits(kVehicle2x2, *t, grid, 40, 10) == oc::kLand);
    // Off the grid counts as occupied.
    CHECK(grid.ground_at(-1, 0));
    CHECK(grid.water_at(64, 0));
}

TEST_CASE("A footprint's cells start where Moho's ToCellPos puts them", "[occupancy]") {
    // lrint(centre - size / 2): a 2 x 2 at (40, 10) covers cells 39-40, 9-10.
    const OccupancyRect r = osc::sim::footprint_rect(kVehicle2x2, 40.0f, 10.0f);
    CHECK(r.x0 == 39);
    CHECK(r.z0 == 9);
    CHECK(r.x1 == 41);
    CHECK(r.z1 == 11);
    // A 3 x 3 at (40.5, 10.5): lrint(39) and lrint(9).
    const OccupancyRect s = osc::sim::footprint_rect(kAmphibious3x3, 40.5f, 10.5f);
    CHECK(s.x0 == 39);
    CHECK(s.z0 == 9);
}

TEST_CASE("Claims on the ground come and go; a release clears an overlapping one's cells too, "
          "and a load rebuilds them",
          "[occupancy]") {
    LuaGuard g;
    osc::sim::SimState sim(g.L, nullptr);
    sim.set_terrain(terrain(true));
    sim.add_army("ARMY_1", "ARMY_1");
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 3;
    sim.set_game_setup(game);
    GroundOccupant a;
    a.caps = oc::kLand;
    a.rects.push_back({40, 10, 44, 14});
    GroundOccupant b;
    b.caps = oc::kLand;
    b.rects.push_back({42, 12, 46, 16}); // overlaps a's corner
    sim.occupy_ground(EntityId::of(101, 1), a);
    sim.occupy_ground(EntityId::of(102, 1), b);
    CHECK(sim.occupancy().ground_at(41, 11));
    CHECK(sim.occupancy().ground_at(43, 13));
    CHECK(sim.footprint_fits_at(kVehicle2x2, 43.0f, 13.0f) == 0);
    sim.release_ground(EntityId::of(101, 1));
    CHECK_FALSE(sim.occupancy().ground_at(41, 11));
    CHECK_FALSE(sim.occupancy().ground_at(43, 13));
    CHECK(sim.occupancy().ground_at(45, 15));

    // Saved and restored, the grid is made again from what stands on it.
    sim.set_recording(true);
    sim.tick();
    const osc::sim::SavedGame save = osc::sim::save_game(sim, "occupied");
    REQUIRE_FALSE(save.snapshot.empty());
    LuaGuard g2;
    osc::sim::SimState restored(g2.L, nullptr);
    restored.set_terrain(terrain(true));
    restored.add_army("ARMY_1", "ARMY_1");
    restored.set_game_setup(game);
    GroundOccupant boot; // a claim the boot made, which the save replaces
    boot.caps = oc::kLand;
    boot.rects.push_back({0, 0, 4, 4});
    restored.occupy_ground(EntityId::of(7, 1), boot);
    const std::string err = osc::sim::load_snapshot(restored, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK_FALSE(restored.occupancy().ground_at(1, 1));
    CHECK_FALSE(restored.occupancy().ground_at(41, 11));
    CHECK(restored.occupancy().ground_at(43, 13));
    CHECK(restored.occupancy().ground_at(45, 15));
}

namespace {

/// A sim on the test map with unit blueprints registered, as the kill
/// tests make theirs.
struct UnitWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    UnitWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(terrain(true));
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'mex1', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 2, SizeZ = 2}, General = {UpgradesTo = 'mex2'},"
                 " Economy = {BuildRate = 10, BuildableCategory = {'mex2'}},"
                 " Physics = {MotionType = 'RULEUMT_None', BuildOnLayerCaps = {LAYER_Land = "
                 "true}}}",
                 "{BlueprintId = 'mex2', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 2, SizeZ = 2}, General = {UpgradesFrom = 'mex1'},"
                 " Economy = {BuildTime = 10, BuildCostMass = 1, BuildCostEnergy = 1},"
                 " Physics = {MotionType = 'RULEUMT_None', BuildOnLayerCaps = {LAYER_Land = "
                 "true}}}",
                 "{BlueprintId = 'pgen', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Physics = {MotionType = 'RULEUMT_None', BuildOnLayerCaps = {LAYER_Land = "
                 "true}}}",
                 "{BlueprintId = 'gate', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 7, SizeZ = 7},"
                 " Physics = {MotionType = 'RULEUMT_None', BuildOnLayerCaps = {LAYER_Land = true},"
                 "  OccupyRects = {-2.5, 0, 1, 3, 2.5, 0, 1, 3}}}",
                 "{BlueprintId = 'beacon', Categories = {'STRUCTURE', 'FERRYBEACON'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_None'}}",
                 "{BlueprintId = 'tank', Categories = {'LAND'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 1, SizeZ = 1}, Physics = {MotionType = 'RULEUMT_Land'}}",
                 "{BlueprintId = 'sbed', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Physics = {MotionType = 'RULEUMT_None', BuildOnLayerCaps = {LAYER_Seabed = "
                 "true}}}",
                 "{BlueprintId = 'naval', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Physics = {MotionType = 'RULEUMT_None', BuildOnLayerCaps = {LAYER_Water = "
                 "true}}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"mex1", "mex2", "pgen", "gate", "beacon", "tank", "sbed", "naval"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
};

} // namespace

TEST_CASE("An immobile unit claims its ground as it is made, and frees it as it goes",
          "[occupancy]") {
    UnitWorld w;
    const auto& grid = w.sim.occupancy();
    REQUIRE(w.state.do_string("pgen = CreateUnit('pgen', 1, 40, 10, 10)").ok());
    // A 2 x 2 at (40, 10): cells 39-40, 9-10.
    CHECK(grid.ground_at(39, 9));
    CHECK(grid.ground_at(40, 10));
    CHECK_FALSE(grid.ground_at(41, 10));
    // A quantum gateway's two sides, not its 7 x 7 footprint: centre +- 2.5,
    // half sizes 1 and 3 -- cells 37-38 and 42-43 across, 27-32 deep.
    REQUIRE(w.state.do_string("gate = CreateUnit('gate', 1, 40, 10, 30)").ok());
    CHECK(grid.ground_at(37, 27));
    CHECK(grid.ground_at(43, 32));
    CHECK_FALSE(grid.ground_at(40, 30)); // the way through
    // A ferry beacon and a tank claim nothing.
    REQUIRE(w.state
                .do_string("CreateUnit('beacon', 1, 50, 10, 10)"
                           " CreateUnit('tank', 1, 55, 10, 10)")
                .ok());
    CHECK_FALSE(grid.ground_at(49, 9));
    CHECK_FALSE(grid.ground_at(54, 9));
    // Destroyed, the power generator's ground is free once it leaves the sim.
    REQUIRE(w.state.do_string("moho.entity_methods.Destroy(pgen)").ok());
    w.sim.tick();
    CHECK_FALSE(grid.ground_at(39, 9));
    CHECK(grid.ground_at(37, 27));
}

TEST_CASE("A structure's ground freed under another's: the cells they share go free on that grid "
          "whatever the other's caps, and the other grid's claims stand",
          "[occupancy]") {
    UnitWorld w;
    const auto& grid = w.sim.occupancy();
    // In the water west of x = 32: a seabed structure (SEABED), a land one
    // (LAND) over its corner -- other caps, the same ground grid -- and a
    // floating one (WATER) over both. Their cells: 19-20 x 9-10, 20-21 x
    // 10-11 and 20-21 x 9-10.
    REQUIRE(w.state
                .do_string("sbed = CreateUnit('sbed', 1, 20, 0, 10)"
                           " pgen = CreateUnit('pgen', 1, 21, 0, 11)"
                           " naval = CreateUnit('naval', 1, 21, 5, 10)")
                .ok());
    CHECK(grid.ground_at(19, 9));
    CHECK(grid.ground_at(20, 10));
    CHECK(grid.ground_at(21, 11));
    CHECK(grid.water_at(21, 9));
    CHECK_FALSE(grid.water_at(19, 9));
    // The seabed structure goes: its cells are free, the one the land
    // structure shares with it too; the land one's own stay, and the water
    // above is untouched.
    REQUIRE(w.state.do_string("moho.entity_methods.Destroy(sbed)").ok());
    w.sim.tick();
    CHECK_FALSE(grid.ground_at(19, 9));
    CHECK_FALSE(grid.ground_at(20, 9));
    CHECK_FALSE(grid.ground_at(20, 10));
    CHECK(grid.ground_at(21, 10));
    CHECK(grid.ground_at(21, 11));
    CHECK(grid.water_at(20, 9));
    CHECK(grid.water_at(21, 10));
    // The floating one goes: its water is free, the ground under it held.
    REQUIRE(w.state.do_string("moho.entity_methods.Destroy(naval)").ok());
    w.sim.tick();
    CHECK_FALSE(grid.water_at(20, 9));
    CHECK_FALSE(grid.water_at(21, 10));
    CHECK(grid.ground_at(21, 10));
    CHECK(grid.ground_at(20, 11));
    CHECK_FALSE(grid.ground_at(21, 9));
}

TEST_CASE("A structure frees its blueprint's footprint where it stands, not only its OccupyRects",
          "[occupancy]") {
    UnitWorld w;
    const auto& grid = w.sim.occupancy();
    // A power generator on a quantum gateway's way through: cells 39-40 x
    // 29-30, in the gateway's footprint, 37-43 x 27-33.
    REQUIRE(w.state
                .do_string("gate = CreateUnit('gate', 1, 40.5, 10, 30.5)"
                           " pgen = CreateUnit('pgen', 1, 40, 10, 30)")
                .ok());
    CHECK(grid.ground_at(37, 28));
    CHECK(grid.ground_at(39, 29));
    REQUIRE(w.state.do_string("moho.entity_methods.Destroy(gate)").ok());
    w.sim.tick();
    CHECK_FALSE(grid.ground_at(37, 28));
    CHECK_FALSE(grid.ground_at(43, 33));
    CHECK_FALSE(grid.ground_at(39, 29));
    CHECK_FALSE(grid.ground_at(40, 30));
}

TEST_CASE("An upgrade's unit keeps the ground its old one leaves; a cancelled one's goes, and the "
          "old one occupies its own again",
          "[occupancy][upgrade]") {
    UnitWorld w;
    const auto& grid = w.sim.occupancy();
    REQUIRE(w.state
                .do_string("old = CreateUnit('mex1', 1, 40, 10, 10)"
                           " function old:OnStopBuild(built, order)"
                           "   if order == 'Upgrade' and"
                           "       moho.entity_methods.GetFractionComplete(built) >= 1 then"
                           "     moho.entity_methods.Destroy(self)"
                           "   end"
                           " end"
                           " IssueUpgrade({old}, 'mex2')")
                .ok());
    osc::sim::Unit* old = nullptr;
    w.sim.entity_registry().for_each_unit(
        [&](osc::sim::Entity& e) { old = static_cast<osc::sim::Unit*>(&e); });
    REQUIRE(old);
    old->economy().production_mass = 100.0;
    old->economy().production_energy = 100.0;
    old->economy().production_active = true;
    old->economy().natural_producer = true;
    const EntityId old_id = old->entity_id();
    w.sim.tick();
    w.sim.tick();
    REQUIRE(old->is_building());
    const EntityId frame = old->build_target_id();
    const auto gone = [&](EntityId id) { return w.sim.entity_registry().find(id) == nullptr; };

    SECTION("finished") {
        for (int i = 0; i < 40 && !gone(old_id); ++i) {
            w.sim.tick();
        }
        REQUIRE(gone(old_id));
        REQUIRE_FALSE(gone(frame));
    }
    SECTION("cancelled") {
        REQUIRE(w.state.do_string("IssueClearCommands({old})").ok());
        w.sim.tick();
        REQUIRE(gone(frame));
        REQUIRE_FALSE(gone(old_id));
    }
    CHECK(grid.ground_at(39, 9));
    CHECK(grid.ground_at(40, 10));
}

TEST_CASE("A structure given to another army keeps its ground under its replacement",
          "[occupancy]") {
    UnitWorld w;
    const auto& grid = w.sim.occupancy();
    REQUIRE(
        w.state.do_string("pgen = CreateUnit('pgen', 1, 40, 10, 10) ChangeUnitArmy(pgen, 2)").ok());
    w.sim.tick();
    int left = 0;
    w.sim.entity_registry().for_each_unit(
        [&](osc::sim::Entity& e) { left += static_cast<osc::sim::Unit&>(e).army() == 1 ? 1 : 0; });
    REQUIRE(left == 1);
    CHECK(grid.ground_at(39, 9));
    CHECK(grid.ground_at(40, 10));
}

TEST_CASE("A claim with no caps, a tree's or a wreck's, fills nothing, and its release clears "
          "nothing",
          "[occupancy]") {
    LuaGuard g;
    osc::sim::SimState sim(g.L, nullptr);
    sim.set_terrain(terrain(true));
    GroundOccupant land;
    land.caps = oc::kLand;
    land.rects.push_back({40, 10, 44, 14});
    GroundOccupant water;
    water.caps = oc::kWater;
    water.rects.push_back({10, 10, 14, 14});
    sim.occupy_ground(EntityId::of(101, 1), land);
    sim.occupy_ground(EntityId::of(102, 1), water);
    const auto cells = [&] {
        std::vector<bool> out;
        for (osc::i32 z = 0; z < 64; ++z) {
            for (osc::i32 x = 0; x < 64; ++x) {
                out.push_back(sim.occupancy().ground_at(x, z));
                out.push_back(sim.occupancy().water_at(x, z));
            }
        }
        return out;
    };
    const std::vector<bool> claimed = cells();
    // A wreck across both claims and the free ground between them.
    GroundOccupant wreck;
    wreck.rects.push_back({12, 12, 42, 13});
    sim.occupy_ground(EntityId::of(103, 1), wreck);
    CHECK(cells() == claimed);
    sim.release_ground(EntityId::of(103, 1));
    CHECK(cells() == claimed);
    CHECK(sim.ground_occupant_count() == 2);
    CHECK(sim.occupancy().ground_at(41, 12));
    CHECK(sim.occupancy().water_at(12, 12));
    CHECK_FALSE(sim.occupancy().ground_at(20, 12));
    CHECK_FALSE(sim.occupancy().water_at(20, 12));
}

TEST_CASE("The starting layer: air footprints fly, a beacon floats, the rest by the water",
          "[occupancy][layers]") {
    // Moho's Entity::GetStartingLayer with no layer asked for (faf-re), which
    // a unit's creation and Unit::Warp both read
    using osc::sim::starting_layer;
    const auto wet = terrain(true);
    const auto is = [](const char* got, const char* want) { return std::string(got) == want; };
    // An AIR footprint flies wherever it is; an experimental's starts on the ground
    CHECK(is(starting_layer(oc::kAir, false, *wet, 10.0f, 10.0f), "Air"));
    CHECK(is(starting_layer(oc::kAir, false, *wet, 40.0f, 10.0f), "Air"));
    CHECK(is(starting_layer(oc::kAir, true, *wet, 10.0f, 10.0f), "Land"));
    // Dry ground: Land, whatever else
    CHECK(is(starting_layer(oc::kLand | oc::kWater, false, *wet, 40.0f, 10.0f), "Land"));
    CHECK(is(starting_layer(oc::kLand, false, *wet, 40.0f, 10.0f, true), "Land"));
    // Under the water: a sub dives (not an experimental), a WATER footprint or a
    // FERRYBEACON floats, the rest stand on the seabed
    CHECK(is(starting_layer(oc::kSub | oc::kWater, false, *wet, 10.0f, 10.0f), "Sub"));
    CHECK(is(starting_layer(oc::kSub | oc::kWater, true, *wet, 10.0f, 10.0f), "Water"));
    CHECK(is(starting_layer(oc::kLand | oc::kWater, false, *wet, 10.0f, 10.0f), "Water"));
    CHECK(is(starting_layer(oc::kLand, false, *wet, 10.0f, 10.0f, true), "Water"));
    CHECK(is(starting_layer(oc::kLand | oc::kSeabed, false, *wet, 10.0f, 10.0f), "Seabed"));
    // No water on the map: Land
    const auto dry = terrain(false);
    CHECK(is(starting_layer(oc::kLand | oc::kSeabed, false, *dry, 10.0f, 10.0f), "Land"));
}
