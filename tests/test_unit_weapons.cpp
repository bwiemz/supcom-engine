// A unit's weapons, as Moho makes them from its blueprint (faf-re Unit.cpp,
// Unit's constructor; CAiAttackerImpl::CreateWeapon; RUnitBlueprintWeapon's
// defaults, RUnitBlueprint.cpp).

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

struct WeaponWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    WeaponWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        lua_State* L = state.raw();
        for (const char* death : {"", "DummyWeapon = true"}) {
            const std::string id = *death ? "dummy" : "real";
            REQUIRE(state
                        .do_string("return {BlueprintId = '" + id +
                                   "', Defense = {MaxHealth = 100}, Weapon = {"
                                   " {Label = 'Gun'}, {Label = 'Laser', TargetCheckInterval = 0.5},"
                                   " {Label = 'DeathWeapon', WeaponCategory = 'Death', " +
                                   death + "}}}")
                        .ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string(R"(
            Plain = setmetatable({}, {__index = moho.unit_methods})
            Plain.__index = Plain
        )")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"real", "dummy"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    bool check(const char* code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }

    osc::sim::Unit* unit(const char* global) {
        lua_State* L = state.raw();
        lua_getglobal(L, global);
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        auto* u = static_cast<osc::sim::Unit*>(lua_touserdata(L, -1));
        lua_pop(L, 2);
        return u;
    }
};

} // namespace

TEST_CASE("A unit has no weapon for a DummyWeapon entry of its blueprint", "[weapons]") {
    WeaponWorld w;
    REQUIRE(w.check(R"(
        real = CreateUnit('real', 1, 0, 0, 0)
        dummy = CreateUnit('dummy', 1, 5, 0, 0)
    )"));
    CHECK(w.check(R"(
        assert(real:GetWeaponCount() == 3, 'real ' .. real:GetWeaponCount())
        assert(real:GetWeapon(3):GetBlueprint().Label == 'DeathWeapon')
        assert(dummy:GetWeaponCount() == 2, 'dummy ' .. dummy:GetWeaponCount())
        assert(dummy:GetWeapon(1):GetBlueprint().Label == 'Gun')
        assert(dummy:GetWeapon(2):GetBlueprint().Label == 'Laser')
    )"));
    const auto& weapons = w.unit("dummy")->weapons();
    REQUIRE(weapons.size() == 2);
    CHECK(weapons[0]->weapon_index == 0);
    CHECK(weapons[1]->weapon_index == 1);
}

TEST_CASE("A weapon blueprint's TargetCheckInterval left out is Moho's 3 seconds", "[weapons]") {
    WeaponWorld w;
    REQUIRE(w.check("real = CreateUnit('real', 1, 0, 0, 0)"));
    const auto& weapons = w.unit("real")->weapons();
    REQUIRE(weapons.size() == 3);
    CHECK(weapons[0]->target_check_period == 30);
    CHECK(weapons[1]->target_check_period == 5);
}
