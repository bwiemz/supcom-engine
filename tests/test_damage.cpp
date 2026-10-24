#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

struct DamageWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    DamageWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        lua_State* L = state.raw();
        REQUIRE(state
                    .do_string("return {BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                               " Defense = {MaxHealth = 100}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(run(R"(
            Tank = setmetatable({
                OnDamage = function(self, instigator, amount)
                    self.hits = (self.hits or 0) + 1
                end,
                OnKilled = function(self) end,
            }, {__index = moho.unit_methods})
            Tank.__index = Tank
        )"));
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "tank");
        lua_getglobal(L, "Tank");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    bool run(const char* code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }
};

} // namespace

TEST_CASE("Damage passes over a dying unit and DamageArea still reaches it", "[damage]") {
    for (const bool dying : {false, true}) {
        CAPTURE(dying);
        DamageWorld w;
        REQUIRE(w.run(R"(
            shooter = CreateUnit('tank', 1, 0, 0, 0)
            victim = CreateUnit('tank', 2, 5, 0, 0)
        )"));
        if (dying) {
            REQUIRE(w.run("victim:Kill()"));
        }
        REQUIRE(w.run(R"(
            Damage(shooter, {0, 0, 0}, victim, 10, 'Normal')
            point = victim.hits or 0
            DamageArea(shooter, {5, 0, 0}, 2, 10, 'Normal', false)
            area = (victim.hits or 0) - point
        )"));
        CHECK(w.run(dying ? "assert(point == 0, point)" : "assert(point == 1, point)"));
        CHECK(w.run("assert(area == 1, area)"));
    }
}
