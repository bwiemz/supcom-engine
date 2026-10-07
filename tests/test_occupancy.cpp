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

TEST_CASE("Claims on the ground come and go; an overlapping one keeps its cells, and a "
          "load rebuilds them",
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
    sim.occupy_ground(101, a);
    sim.occupy_ground(102, b);
    CHECK(sim.occupancy().ground_at(41, 11));
    CHECK(sim.occupancy().ground_at(43, 13));
    CHECK(sim.footprint_fits_at(kVehicle2x2, 43.0f, 13.0f) == 0);
    sim.release_ground(101);
    CHECK_FALSE(sim.occupancy().ground_at(41, 11));
    CHECK(sim.occupancy().ground_at(43, 13)); // b still stands there
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
    restored.occupy_ground(7, boot);
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
        lua_State* L = state.raw();
        for (const char* bp : {
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
        for (const char* id : {"pgen", "gate", "beacon", "tank"}) {
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
