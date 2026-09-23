// What an entity owns outside the registry must be released however it is
// removed: its structure footprint on the pathfinding grid, and its Lua
// table's _c_object pointer back to the (about to be freed) C++ object.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::map::CellPassability;
using osc::map::Heightmap;
using osc::map::PathfindingGrid;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr osc::u32 kMapSize = 64;

Heightmap flat_heightmap() {
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1000);
    return Heightmap(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
}

CellPassability cell_at(const PathfindingGrid& grid, osc::f32 x, osc::f32 z) {
    osc::u32 gx = 0, gz = 0;
    grid.world_to_grid(x, z, gx, gz);
    return grid.get(gx, gz);
}

Unit* spawn_structure(SimState& sim, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_position({x, 0.0f, z});
    u->add_category("STRUCTURE");
    u->set_footprint_size(4.0f, 4.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

} // namespace

TEST_CASE("obstacle cells stay blocked while any footprint covers them",
          "[nav][m183]") {
    PathfindingGrid grid(flat_heightmap(), 0.0f, false, /*cell_size=*/2);
    // Two 4x4 footprints whose rectangles touch at x = 12: after rounding
    // both cover the grid column there.
    grid.mark_obstacle(10.0f, 10.0f, 4.0f, 4.0f); // x 8..12
    grid.mark_obstacle(14.0f, 10.0f, 4.0f, 4.0f); // x 12..16
    REQUIRE(cell_at(grid, 12.0f, 10.0f) == CellPassability::Obstacle);

    grid.clear_obstacle(10.0f, 10.0f, 4.0f, 4.0f);
    CHECK(cell_at(grid, 9.0f, 10.0f) == CellPassability::Passable);
    CHECK(cell_at(grid, 12.0f, 10.0f) == CellPassability::Obstacle); // shared
    CHECK(cell_at(grid, 15.0f, 10.0f) == CellPassability::Obstacle);

    grid.clear_obstacle(14.0f, 10.0f, 4.0f, 4.0f);
    CHECK(cell_at(grid, 12.0f, 10.0f) == CellPassability::Passable);
    CHECK(cell_at(grid, 15.0f, 10.0f) == CellPassability::Passable);

    // Clearing again (or clearing what was never marked) is harmless.
    grid.clear_obstacle(14.0f, 10.0f, 4.0f, 4.0f);
    CHECK(cell_at(grid, 15.0f, 10.0f) == CellPassability::Passable);
}

TEST_CASE("a removed structure stops blocking paths", "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(flat_heightmap(), 0.0f, false));
    sim.build_pathfinding_grid();
    auto& grid = *sim.pathfinding_grid();

    auto* factory = spawn_structure(sim, 20.0f, 20.0f);
    const osc::u32 id = factory->entity_id();
    sim.occupy_footprint(*factory);
    sim.occupy_footprint(*factory); // idempotent
    REQUIRE(sim.occupies_footprint(id));
    REQUIRE(cell_at(grid, 20.0f, 20.0f) == CellPassability::Obstacle);

    // Any removal path (reclaim, sacrifice, Lua Destroy) ends here.
    sim.entity_registry().unregister_entity(id);
    CHECK_FALSE(sim.occupies_footprint(id));
    CHECK(cell_at(grid, 20.0f, 20.0f) == CellPassability::Passable);
}

TEST_CASE("non-structures never occupy the grid", "[nav][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(flat_heightmap(), 0.0f, false));
    sim.build_pathfinding_grid();

    auto u = std::make_unique<Unit>();
    u->set_position({30.0f, 0.0f, 30.0f});
    u->set_footprint_size(2.0f, 2.0f); // mobile unit: no STRUCTURE category
    auto* tank = u.get();
    sim.entity_registry().register_entity(std::move(u));
    sim.occupy_footprint(*tank);
    CHECK_FALSE(sim.occupies_footprint(tank->entity_id()));
    CHECK(cell_at(*sim.pathfinding_grid(), 30.0f, 30.0f) == CellPassability::Passable);
}

TEST_CASE("C++-side removal severs the Lua table's pointer to the entity",
          "[lifetime][m183]") {
    LuaGuard g;
    lua_State* L = g.L;
    SimState sim(L, nullptr);

    auto u = std::make_unique<Unit>();
    auto* raw = u.get();
    const osc::u32 id = sim.entity_registry().register_entity(std::move(u));

    // The unit's Lua table, as create_unit_core builds it.
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "unit_table"); // a script keeps a handle
    raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // e.g. reclaim / sacrifice / projectile impact -- no entity_Destroy.
    sim.entity_registry().unregister_entity(id);

    lua_getglobal(L, "unit_table");
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    CHECK(lua_type(L, -1) == LUA_TLIGHTUSERDATA);
    CHECK(lua_touserdata(L, -1) == nullptr); // stale handle reads as destroyed
    lua_pop(L, 2);
}

TEST_CASE("a crashed aircraft is removed from the registry", "[lifetime][m183]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(flat_heightmap(), 0.0f, false));
    sim.build_pathfinding_grid();

    auto u = std::make_unique<Unit>();
    u->set_layer("Air");
    u->set_position({32.0f, 40.0f, 32.0f});
    u->set_current_altitude(40.0f);
    u->set_current_airspeed(10.0f);
    auto* plane = u.get();
    const osc::u32 id = sim.entity_registry().register_entity(std::move(u));
    const size_t before = sim.entity_registry().count();

    plane->begin_air_crash(50.0f);
    for (int t = 0; t < 300 && sim.entity_registry().find(id); ++t) sim.tick();

    CHECK(sim.entity_registry().find(id) == nullptr); // impacted and removed
    CHECK(sim.entity_registry().count() == before - 1);
}

TEST_CASE("an expired projectile's Lua table no longer points at it",
          "[lifetime]") {
    // UEF build effects keep projectile handles in a trash bag and Destroy
    // them from Lua after the projectile's own lifetime has run out.
    LuaGuard g;
    lua_State* L = g.L;
    SimState sim(L, nullptr);

    auto p = std::make_unique<osc::sim::Projectile>();
    p->lifetime = 0.05f;
    auto* raw = p.get();
    const osc::u32 id = sim.entity_registry().register_entity(std::move(p));
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "proj_table");
    raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    raw->update(0.1, sim.entity_registry(), L, nullptr); // lifetime runs out
    REQUIRE(sim.entity_registry().find(id) == nullptr);

    lua_getglobal(L, "proj_table");
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    CHECK(lua_touserdata(L, -1) == nullptr);
    lua_pop(L, 2);
}
