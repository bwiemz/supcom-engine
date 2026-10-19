#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "sim/game_setup.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::f32;

namespace {

constexpr f32 kScale = 1.0f / 128.0f;

/// A 64 x 64 map sloping up eastwards, a unit of height each grid point.
std::unique_ptr<osc::map::Terrain> slope() {
    std::vector<osc::u16> heights(65 * 65);
    for (int z = 0; z <= 64; ++z) {
        for (int x = 0; x <= 64; ++x) {
            heights[static_cast<size_t>(z) * 65 + static_cast<size_t>(x)] =
                static_cast<osc::u16>(static_cast<f32>(x) / kScale);
        }
    }
    return std::make_unique<osc::map::Terrain>(osc::map::Heightmap(64, 64, kScale, heights), 0.0f,
                                               false);
}

/// A flat 64 x 64 seabed at height 0 under water 4 deep.
std::unique_ptr<osc::map::Terrain> sea() {
    std::vector<osc::u16> heights(65 * 65, 0);
    return std::make_unique<osc::map::Terrain>(osc::map::Heightmap(64, 64, kScale, heights), 4.0f,
                                               true);
}

struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit World(std::unique_ptr<osc::map::Terrain> terrain = slope()) {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(std::move(terrain));
        sim.add_army("ARMY_1", "ARMY_1");
    }

    /// The default pathing's grid cell (gx, gz), for a land unit
    bool land_passes(osc::u32 gx, osc::u32 gz) const {
        return sim.pathfinding_grid()->is_passable_for(gx, gz, "Land");
    }

    f32 height(int x, int z) const {
        return sim.terrain()->heightmap().get_height_at_grid(static_cast<osc::u32>(x),
                                                             static_cast<osc::u32>(z));
    }
};

} // namespace

TEST_CASE("FlattenMapRect sets the grid points from (x, z) to (x + sx, z + sz)", "[terrain]") {
    World w;
    REQUIRE(w.state.do_string("FlattenMapRect(10, 20, 3, 2, 7.5)").ok());
    for (int z = 20; z <= 22; ++z) {
        for (int x = 10; x <= 13; ++x) {
            INFO(x << ", " << z);
            CHECK(w.height(x, z) == 7.5f);
        }
    }
    CHECK(w.height(9, 21) == 9.0f);
    CHECK(w.height(14, 21) == 14.0f);
    CHECK(w.height(11, 19) == 11.0f);
    CHECK(w.height(11, 23) == 11.0f);
    CHECK(w.sim.terrain()->get_terrain_height(11.5f, 21.5f) == 7.5f);
}

TEST_CASE("FlattenMapRect stops short of the map's last points and truncates to its units",
          "[terrain]") {
    World w;
    REQUIRE(w.state.do_string("FlattenMapRect(62, 62, 4, 4, 3.0039)").ok());
    CHECK(w.height(62, 62) == 3.0f);
    CHECK(w.height(63, 63) == 3.0f);
    CHECK(w.height(64, 63) == 64.0f);
    CHECK(w.height(63, 64) == 63.0f);
    REQUIRE(w.state.do_string("FlattenMapRect(64, 10, 1, 1, 3)").ok());
    CHECK(w.height(64, 10) == 64.0f);
    CHECK(w.sim.terrain()->flattenings().size() == 1);
}

TEST_CASE("FlattenMapRect sets a land unit about the rect on the new ground", "[terrain]") {
    World w;
    lua_State* L = w.state.raw();
    REQUIRE(w.state
                .do_string("return {BlueprintId = 'tank', Categories = {'LAND'},"
                           " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                           " Physics = {MotionType = 'RULEUMT_Land'}}")
                .ok());
    w.store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
    lua_pop(L, 1);
    w.store.expose_to_lua(L);
    REQUIRE(w.state
                .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                           " Plain.__index = Plain")
                .ok());
    lua_pushstring(L, "__osc_unit_script_classes");
    lua_newtable(L);
    lua_pushstring(L, "tank");
    lua_getglobal(L, "Plain");
    lua_rawset(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX);
    REQUIRE(w.state.do_string("CreateUnit('tank', 1, 30.5, 30, 30.5)").ok());
    REQUIRE(w.state.do_string("FlattenMapRect(28, 28, 4, 4, 12)").ok());
    int tanks = 0;
    w.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        ++tanks;
        CHECK(e.pending_position().y == 12.0f);
    });
    CHECK(tanks == 1);
}

TEST_CASE("A load restores the saved game's flattened ground over the boot's", "[terrain]") {
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 3;
    World saved;
    saved.sim.set_game_setup(game);
    REQUIRE(saved.state.do_string("FlattenMapRect(10, 10, 2, 2, 4)").ok());
    lua_settop(saved.state.raw(), 0);
    saved.sim.set_recording(true);
    saved.sim.tick();
    const osc::sim::SavedGame save = osc::sim::save_game(saved.sim, "flattened");
    REQUIRE_FALSE(save.snapshot.empty());

    World loaded;
    loaded.sim.set_game_setup(game);
    REQUIRE(loaded.state.do_string("FlattenMapRect(40, 40, 2, 2, 1)").ok());
    lua_settop(loaded.state.raw(), 0);
    const std::string err = osc::sim::load_snapshot(loaded.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(loaded.height(11, 11) == 4.0f);
    CHECK(loaded.height(41, 41) == 41.0f);
    CHECK(loaded.sim.terrain()->flattenings().size() == 1);
}

TEST_CASE("Flattened ground is the look-ahead's highest ground, until it is reset", "[terrain]") {
    World w;
    const auto& hm = w.sim.terrain()->heightmap();
    const f32 before = hm.look_ahead_max(11.0f, 11.0f, 8.0f);
    REQUIRE(before < 100.0f);
    REQUIRE(w.state.do_string("FlattenMapRect(10, 10, 2, 2, 100)").ok());
    CHECK(hm.look_ahead_max(11.0f, 11.0f, 8.0f) == 100.0f);
    CHECK(hm.look_ahead_max(11.0f, 11.0f, 64.0f) == 100.0f);
    w.sim.terrain()->set_flattenings({});
    CHECK(hm.look_ahead_max(11.0f, 11.0f, 8.0f) == before);
}

TEST_CASE("FlattenMapRect classes the default pathing's cells under it afresh", "[terrain]") {
    // The slope rises a unit a point, too steep for a cell 2 points across
    // (0.75 a unit); flattened, cell (5, 5), points 10-12, is level.
    World w;
    w.sim.build_pathfinding_grid();
    REQUIRE_FALSE(w.land_passes(5, 5));
    const auto before = w.sim.pathfinding_grid()->version();
    REQUIRE(w.state.do_string("FlattenMapRect(10, 10, 2, 2, 11)").ok());
    CHECK(w.land_passes(5, 5));
    CHECK(w.sim.pathfinding_grid()->version() != before); // the pathfinder's labels go stale
    CHECK_FALSE(w.land_passes(8, 5));                     // away from it, as it was
}

TEST_CASE("FlattenMapRect's raised seabed shuts a ship whose draft it no longer has", "[terrain]") {
    World w(sea());
    w.sim.build_pathfinding_grid();
    const auto& grid = *w.sim.pathfinding_grid();
    REQUIRE(grid.water_depth(5, 5) == 4.0f);
    REQUIRE(grid.is_passable_for(5, 5, "Water", 2.0f, false));
    REQUIRE(w.sim.flatten_map_rect(10, 10, 2, 2, 3.0f));
    CHECK(grid.water_depth(5, 5) == 1.0f);
    CHECK_FALSE(grid.is_passable_for(5, 5, "Water", 2.0f, false));
    CHECK(grid.is_passable_for(5, 5, "Water", 0.5f, false));
}

TEST_CASE("A structure on flattened ground still blocks it, and leaves the new ground",
          "[terrain]") {
    World w;
    w.sim.build_pathfinding_grid();
    auto& grid = *w.sim.pathfinding_grid();
    grid.mark_obstacle(11.0f, 11.0f, 2.0f, 2.0f);
    REQUIRE(w.state.do_string("FlattenMapRect(10, 10, 2, 2, 11)").ok());
    CHECK_FALSE(w.land_passes(5, 5));
    CHECK(grid.terrain_passable_for(5, 5, "Land", 0.0f, false));
    grid.clear_obstacle(11.0f, 11.0f, 2.0f, 2.0f);
    CHECK(w.land_passes(5, 5));
}

TEST_CASE("A load's ground is the default pathing's, not the boot's", "[terrain]") {
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 3;
    World saved;
    saved.sim.build_pathfinding_grid();
    saved.sim.set_game_setup(game);
    REQUIRE(saved.state.do_string("FlattenMapRect(10, 10, 2, 2, 11)").ok());
    lua_settop(saved.state.raw(), 0);
    saved.sim.set_recording(true);
    saved.sim.tick();
    const osc::sim::SavedGame save = osc::sim::save_game(saved.sim, "flattened");

    World loaded;
    loaded.sim.build_pathfinding_grid();
    loaded.sim.set_game_setup(game);
    REQUIRE(loaded.state.do_string("FlattenMapRect(40, 40, 2, 2, 41)").ok());
    lua_settop(loaded.state.raw(), 0);
    REQUIRE(loaded.land_passes(20, 20));
    const std::string err = osc::sim::load_snapshot(loaded.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(loaded.land_passes(5, 5));         // the saved game's flattening
    CHECK_FALSE(loaded.land_passes(20, 20)); // the boot's, undone
}

TEST_CASE("Ground alone changes the sync checksum, in its domain, and a load restores it",
          "[terrain][checksum]") {
    World a;
    World b;
    const auto before = a.sim.checksum_parts();
    REQUIRE(before.values() == b.sim.checksum_parts().values());
    // Far from anything: only the ground changes.
    REQUIRE(a.state.do_string("FlattenMapRect(50, 50, 2, 2, 3)").ok());
    lua_settop(a.state.raw(), 0);
    const auto after = a.sim.checksum_parts();
    for (size_t i = 0; i < osc::sim::SimState::ChecksumParts::kCount; ++i) {
        INFO(osc::sim::SimState::ChecksumParts::kNames[i]);
        const bool terrain = std::string(osc::sim::SimState::ChecksumParts::kNames[i]) == "terrain";
        CHECK((after.values()[i] != before.values()[i]) == terrain);
    }
    // Another flattening is other ground.
    REQUIRE(b.state.do_string("FlattenMapRect(50, 50, 2, 2, 4)").ok());
    lua_settop(b.state.raw(), 0);
    CHECK(b.sim.checksum_parts().terrain != after.terrain);
    // The ground reset is the map's again.
    b.sim.terrain()->set_flattenings({});
    CHECK(b.sim.checksum_parts().terrain == before.terrain);

    // A saved game's ground, loaded over other ground, checksums as it did.
    osc::sim::GameSetup game;
    game.scenario = "/maps/test/test_scenario.lua";
    game.seed = 3;
    a.sim.set_game_setup(game);
    a.sim.set_recording(true);
    a.sim.tick();
    const auto saved_terrain = a.sim.checksum_parts().terrain;
    const osc::sim::SavedGame save = osc::sim::save_game(a.sim, "ground");
    World c;
    c.sim.set_game_setup(game);
    REQUIRE(c.state.do_string("FlattenMapRect(20, 20, 3, 3, 9)").ok());
    lua_settop(c.state.raw(), 0);
    REQUIRE(c.sim.checksum_parts().terrain != saved_terrain);
    const std::string err = osc::sim::load_snapshot(c.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    CHECK(c.sim.checksum_parts().terrain == saved_terrain);
}
