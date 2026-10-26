// A unit's terrain type, as Moho's Unit::UpdateTerrainType keeps it (faf-re
// Unit.cpp): the TypeCode of the cell at its footprint's corner, looked at as
// it is made, each tick it moves (after UpdateCurrentLayer, for a unit that
// can't fly), and after a Warp; a change calls its script's
// OnTerrainTypeChange(new, old), the two /lua/TerrainTypes.lua entries.
// Retail's Unit.lua remakes its movement effects for the new ground there.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;
using osc::test::tick_until;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void run(lua_State* L, const char* code) {
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "test") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
}

/// 128 x 128 and flat: 'Default' (1) west of x = 64, 'Sand' (5) east of it.
/// The scripts' TerrainTypes come from an import of their own.
void sands(SimState& sim, lua_State* L) {
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::vector<osc::u16>(129 * 129, 0));
    auto terrain = std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false);
    std::vector<osc::u8> types(128 * 128, 1);
    for (size_t z = 0; z < 128; ++z)
        for (size_t x = 64; x < 128; ++x) types[z * 128 + x] = 5;
    terrain->set_terrain_types(std::move(types));
    sim.set_terrain(std::move(terrain));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
    run(L, "function import(path) return {TerrainTypes = {"
           "{TypeCode = 1, Name = 'Default'}, {TypeCode = 5, Name = 'Sand'}}} end "
           "heard = {}");
}

/// A tank whose script hears its terrain type change ("new<old" in `heard`).
Unit* tank(SimState& sim, lua_State* L, f32 x) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_motion_type("RULEUMT_Land");
    u->set_position({x, 0.0f, 64.0f});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    // (a bare lua_open state has no table library)
    const char* code = "return {OnTerrainTypeChange = function(self, new, old) "
                       "local n = 1 while heard[n] do n = n + 1 end "
                       "heard[n] = new.Name .. '<' .. old.Name end}";
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "tank") == 0);
    REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
    u->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    // As it is made: the type under it, no script yet to hear it
    u->update_terrain_type(sim.terrain(), u->position(), nullptr);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

std::string heard(lua_State* L) {
    std::string out;
    lua_getglobal(L, "heard");
    for (int i = 1;; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        out += (out.empty() ? "" : ",") + std::string(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return out;
}

UnitCommand move_to(Vector3 at) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = at;
    return c;
}

} // namespace

TEST_CASE("A unit's script hears its terrain type change as it drives onto new ground",
          "[terrain-type]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sands(sim, g.L);
    Unit* u = tank(sim, g.L, 40.0f);
    CHECK(u->current_terrain_type() == 1);

    // Standing still, nothing is looked at again
    for (int t = 0; t < 10; ++t) sim.tick();
    CHECK(heard(g.L).empty());

    u->push_command(move_to({90.0f, 0.0f, 64.0f}), true);
    tick_until([&] { sim.tick(); }, "the tank finishes its move onto the sand", 300,
               [&] { return u->command_queue().empty() && u->position().x > 85.0f; });
    CHECK(u->current_terrain_type() == 5);
    CHECK(heard(g.L) == "Sand<Default"); // once, on the tick after it crossed
}

TEST_CASE("A Warp looks at the terrain type where the unit lands", "[terrain-type]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sands(sim, g.L);
    Unit* u = tank(sim, g.L, 100.0f);
    CHECK(u->current_terrain_type() == 5);
    u->warp(sim.terrain(), {20.0f, 0.0f, 64.0f}, g.L);
    CHECK(u->current_terrain_type() == 1);
    CHECK(heard(g.L) == "Default<Sand");
}

TEST_CASE("A unit's terrain type is the cell at its footprint's corner", "[terrain-type]") {
    // Moho samples (x - SizeX / 2, z - SizeZ / 2), each rounded to the nearest
    // cell, so a unit's centre is past the edge before its corner is.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sands(sim, g.L);
    Unit* u = tank(sim, g.L, 40.0f);
    osc::blueprints::UnitFootprints wide;
    wide.main.size_x = 4;
    wide.main.size_z = 4;
    u->set_footprints(wide);
    u->update_terrain_type(sim.terrain(), {65.6f, 0.0f, 64.0f}, nullptr);
    CHECK(u->current_terrain_type() == 5); // its corner at 63.6, to the nearest 64: Sand
    u->update_terrain_type(sim.terrain(), {65.4f, 0.0f, 64.0f}, nullptr);
    CHECK(u->current_terrain_type() == 1); // 63.4: 63, Default, its centre past the edge
    // Off the map: Default
    u->update_terrain_type(sim.terrain(), {-5.0f, 0.0f, 64.0f}, nullptr);
    CHECK(u->current_terrain_type() == 1);
}
