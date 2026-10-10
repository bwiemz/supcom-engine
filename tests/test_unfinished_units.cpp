#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstring>
#include <memory>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void flat(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
}

Unit* walker(SimState& sim, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_position({x, 0.0f, z});
    u->set_max_health(100.0f);
    u->set_health(50.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

Unit* unfinished(SimState& sim, lua_State* L, osc::f32 health) {
    const char* code = "__blueprints = {shed = {Economy = {BuildTime = 10, BuildCostMass = 5,"
                       " BuildCostEnergy = 20}}}"
                       " decayed = 0"
                       " return {OnDecayed = function(self) decayed = decayed + 1 end}";
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "shed") == 0);
    REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
    Unit* u = walker(sim, 10.0f, 10.0f);
    u->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    u->set_unit_id("shed");
    u->set_is_being_built(true);
    u->set_health(health);
    u->set_fraction_complete(health / u->max_health());
    u->set_creation_tick(sim.tick_count());
    return u;
}

int decayed(lua_State* L) {
    lua_getglobal(L, "decayed");
    const int n = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return n;
}

osc::sim::UnitCommand move_to(osc::f32 x, osc::f32 z) {
    osc::sim::UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("A unit being built produces nothing and stores nothing until it is finished",
          "[army][economy]") {
    osc::sim::EntityRegistry registry;
    osc::sim::ArmyBrain brain;
    brain.set_index(0);
    auto pgen = std::make_unique<Unit>();
    pgen->set_army(0);
    pgen->economy().production_energy = 20.0;
    pgen->economy().storage_energy = 1000.0;
    pgen->economy().production_active = true;
    pgen->set_is_being_built(true);
    Unit* p = pgen.get();
    registry.register_entity(std::move(pgen));

    brain.update_economy(registry, 0.1);
    CHECK(brain.economy().energy.income == 0.0);
    CHECK(brain.economy().energy.max_storage == 200.0);

    p->set_is_being_built(false);
    brain.update_economy(registry, 0.1);
    CHECK(brain.economy().energy.income == 20.0);
    CHECK(brain.economy().energy.max_storage == 1200.0);
}

TEST_CASE("A unit being built doesn't regenerate", "[sim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    sim.add_army("ARMY_1", "ARMY_1");
    Unit* u = walker(sim, 10.0f, 10.0f);
    u->set_regen_rate(10.0f);
    u->set_is_being_built(true);
    for (int i = 0; i < 10; ++i) {
        sim.tick();
    }
    CHECK(u->health() == 50.0f);
    u->set_is_being_built(false);
    sim.tick();
    CHECK(u->health() > 50.0f);
}

TEST_CASE("An unfinished unit no one builds decays from its second tick to OnDecayed",
          "[sim][build]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    sim.add_army("ARMY_1", "ARMY_1");
    Unit* u = unfinished(sim, g.L, 0.8f);
    sim.tick();
    CHECK(u->health() == 0.8f);
    sim.tick();
    CHECK(u->health() == Catch::Approx(0.3f));
    CHECK(u->fraction_complete() == Catch::Approx(0.003f));
    CHECK(decayed(g.L) == 0);
    sim.tick();
    CHECK(u->health() == 0.0f);
    CHECK(decayed(g.L) == 1);
}

TEST_CASE("A builder on an unfinished unit keeps it from decaying, paused too", "[sim][build]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    sim.add_army("ARMY_1", "ARMY_1");
    Unit* u = unfinished(sim, g.L, 50.0f);
    Unit* builder = walker(sim, 12.0f, 10.0f);
    builder->set_build_target_id(u->entity_id());
    builder->set_paused(true);
    for (int i = 0; i < 10; ++i) {
        sim.tick();
    }
    CHECK(u->health() == 50.0f);
    builder->set_build_target_id(0);
    sim.tick();
    CHECK(u->health() == 50.0f);
    sim.tick();
    CHECK(u->health() < 50.0f);
}

TEST_CASE("A unit being built holds its orders until it is finished", "[sim][orders]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    sim.add_army("ARMY_1", "ARMY_1");
    Unit* u = walker(sim, 10.0f, 10.0f);
    u->set_is_being_built(true);
    u->push_command(move_to(60.0f, 10.0f), true);
    for (int i = 0; i < 20; ++i) {
        sim.tick();
    }
    CHECK(u->position().x == 10.0f);
    CHECK(u->command_queue().size() == 1);

    u->set_is_being_built(false);
    for (int i = 0; i < 20; ++i) {
        sim.tick();
    }
    CHECK(u->position().x > 10.0f);
}

TEST_CASE("Of the units being built, only a factory takes orders", "[sim][orders]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    sim.add_army("ARMY_1", "ARMY_1");
    Unit* tank = walker(sim, 10.0f, 10.0f);
    tank->set_is_being_built(true);
    Unit* factory = walker(sim, 30.0f, 10.0f);
    factory->add_category("FACTORY");
    factory->set_is_being_built(true);

    CHECK(sim.route_command({tank->entity_id()}, move_to(60.0f, 10.0f), true) == 0);
    CHECK(tank->command_queue().empty());
    CHECK(sim.route_command({factory->entity_id()}, move_to(60.0f, 10.0f), true) != 0);
    CHECK(factory->command_queue().size() == 1);
}

TEST_CASE("Of the units being built, only a factory can be selected", "[selection]") {
    Unit tank;
    tank.add_category("SELECTABLE");
    tank.set_is_being_built(true);
    CHECK_FALSE(osc::renderer::selectable(tank));
    Unit factory;
    factory.add_category("FACTORY");
    factory.set_is_being_built(true);
    CHECK(osc::renderer::selectable(factory));
}

TEST_CASE("FAF: a CQUEMOV unit being built takes orders and can be selected",
          "[sim][orders][selection]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    sim.add_army("ARMY_1", "ARMY_1");
    Unit* extractor = walker(sim, 10.0f, 10.0f);
    extractor->add_category("SELECTABLE");
    extractor->add_category("CQUEMOV");
    extractor->set_is_being_built(true);

    CHECK(osc::renderer::selectable(*extractor));
    CHECK(sim.route_command({extractor->entity_id()}, move_to(60.0f, 10.0f), true) != 0);
    CHECK(extractor->command_queue().size() == 1);
}

TEST_CASE("A unit is made with its consumption off, for its script to switch on",
          "[sim][economy]") {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    SimState sim{state.raw(), &store};
    osc::lua::register_moho_bindings(state, sim);
    osc::lua::register_sim_bindings(state, sim);
    sim.add_army("ARMY_1", "ARMY_1");
    lua_State* L = state.raw();
    REQUIRE(state
                .do_string("return {BlueprintId = 'radar', Categories = {'STRUCTURE'},"
                           " Defense = {MaxHealth = 100}, Economy = {BuildTime = 10,"
                           " MaintenanceConsumptionPerSecondEnergy = 15,"
                           " ProductionPerSecondEnergy = 5}}")
                .ok());
    store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
    lua_settop(L, 0);
    store.expose_to_lua(L);

    lua_pushstring(L, "__osc_create_building_unit");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "radar");
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 10);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 10);
    REQUIRE(lua_pcall(L, 5, 2, 0) == 0);
    lua_setglobal(L, "radar");
    auto* radar =
        static_cast<Unit*>(sim.entity_registry().find(static_cast<osc::u32>(lua_tonumber(L, -1))));
    lua_settop(L, 0);
    REQUIRE(radar);
    CHECK_FALSE(radar->economy().consumption_active);
    REQUIRE(state
                .do_string("local d = moho.unit_methods.GetEconData(radar)"
                           " assert(d.energyProduced == 0, d.energyProduced)")
                .ok());
}
