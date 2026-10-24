#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "log_capture.hpp"

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>

using Catch::Approx;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct SiloSim {
    osc::lua::LuaState state;
    SimState sim{state.raw(), nullptr};
    Unit* silo = nullptr;

    SiloSim() {
        osc::lua::register_moho_bindings(state, sim);
        REQUIRE(state
                    .do_string("__blueprints = {nuke = {Economy = {BuildTime = 450000,"
                               " BuildCostEnergy = 1, BuildCostMass = 1}}}")
                    .ok());
        auto u = std::make_unique<Unit>();
        u->set_build_rate(1500.0f);
        auto launcher = std::make_unique<osc::sim::Weapon>();
        launcher->counted_projectile = true;
        launcher->nuke_weapon = true;
        launcher->max_projectile_storage = 5;
        launcher->projectile_bp_id = "nuke";
        u->add_weapon(std::move(launcher));
        silo = u.get();
        sim.entity_registry().register_entity(std::move(u));
        lua_State* L = state.raw();
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, silo);
        lua_rawset(L, -3);
        lua_setglobal(L, "silo");
        REQUIRE(state.do_string("setmetatable(silo, {__index = moho.unit_methods})").ok());
    }
};

} // namespace

TEST_CASE("GiveNukeSiloAmmo(blocks, true) sets the missile's done blocks, not ammo", "[silo]") {
    SiloSim s;
    s.silo->order_silo_build(true);
    REQUIRE(s.state.do_string("silo:GiveNukeSiloAmmo(1050, true)").ok());
    CHECK(s.silo->nuke_silo_ammo() == 0);

    s.silo->update_silo(0.1, 1.0f, s.state.raw());
    REQUIRE(s.silo->silo_building());
    CHECK(s.silo->silo_build().progress == Approx(0.35));

    REQUIRE(s.state.do_string("silo:GiveNukeSiloAmmo(1500, true)").ok());
    CHECK(s.silo->silo_build().progress == Approx(0.5));
    CHECK(s.silo->nuke_silo_ammo() == 0);

    REQUIRE(s.state.do_string("silo:GiveNukeSiloAmmo(2)").ok());
    CHECK(s.silo->nuke_silo_ammo() == 2);
}

TEST_CASE("A Stop turns a silo's auto mode off and drops its missile under way", "[silo]") {
    const bool by_player = GENERATE(false, true);
    CAPTURE(by_player);
    SiloSim s;
    lua_State* L = s.state.raw();
    lua_getglobal(L, "silo");
    s.silo->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    REQUIRE(s.state
                .do_string("__heard = ''\n"
                           "function silo:OnAutoModeOff() __heard = __heard .. 'off' end")
                .ok());
    s.silo->set_auto_mode(true);
    s.silo->order_silo_build(true);
    s.silo->update_silo(0.1, 1.0f, L);
    REQUIRE(s.silo->silo_building());

    osc::sim::UnitCommand stop;
    stop.type = osc::sim::CommandType::Stop;
    if (by_player) {
        s.sim.schedule_command(0, {s.silo->entity_id()}, stop, true);
    } else {
        s.silo->push_command(stop, false);
    }
    s.sim.tick();

    CHECK(s.silo->command_queue().empty());
    CHECK_FALSE(s.silo->auto_mode());
    CHECK_FALSE(s.silo->silo_building());
    CHECK(s.silo->silo_build_count(true) == 0);
    lua_getglobal(L, "__heard");
    CHECK(std::string(lua_tostring(L, -1)) == "off");
    lua_pop(L, 1);
}

TEST_CASE("A silo's build cost script that raises is reported, and its default stands", "[silo]") {
    SiloSim s;
    lua_State* L = s.state.raw();
    lua_getglobal(L, "silo");
    s.silo->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    REQUIRE(s.state
                .do_string("function silo:GetEnergyBuildAdjMod() error('boom') end\n"
                           "function silo:GetMassBuildAdjMod() return 0.5 end")
                .ok());
    s.silo->order_silo_build(true);
    // Moho's RunScript logs the script's error (LogScriptWarning)
    osc::test::LogCapture log;
    s.silo->update_silo(0.1, 1.0f, L);
    REQUIRE(s.silo->silo_building());
    CHECK(log.count("Error running GetEnergyBuildAdjMod script in Entity ") == 1);
    CHECK(log.count("boom") == 1);
    CHECK(log.count("GetMassBuildAdjMod") == 0);
    // Energy at the default (1), mass at the script's half
    const double per_second = 1500.0 / 450000.0;
    CHECK(s.silo->economy().silo_energy == Approx(per_second));
    CHECK(s.silo->economy().silo_mass == Approx(0.5 * per_second));
}
