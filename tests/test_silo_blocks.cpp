#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
