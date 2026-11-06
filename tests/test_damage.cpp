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

    void register_boxed_crate() {
        sim.entity_registry().init_spatial_grid(512, 512);
        lua_State* L = state.raw();
        REQUIRE(
            state
                .do_string("return {BlueprintId = 'crate', Categories = {'LAND'},"
                           " Defense = {MaxHealth = 100}, SizeX = 0.5, SizeY = 0.5, SizeZ = 0.5}")
                .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushstring(L, "crate");
        lua_getglobal(L, "Tank");
        lua_rawset(L, -3);
        lua_pop(L, 1);
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

TEST_CASE("A blast reaches units in its collision cells' order, not by id", "[damage]") {
    DamageWorld w;
    w.register_boxed_crate();
    REQUIRE(w.run(R"(
        order = {}
        Tank.OnDamage = function(self) table.insert(order, self.tag) end
        local function crate(tag, x, z)
            local u = CreateUnit('crate', 2, x, 0, z)
            u.tag = tag
        end
        crate('below', 10, 14)
        crate('first', 10, 10)
        crate('second', 10.5, 10.5)
        crate('along', 14, 10)
        DamageArea(nil, {12, 0, 12}, 6, 1, 'Normal', true, true)
        area = table.concat(order, ' ')
        order = {}
        DamageRing(nil, {12, 0, 12}, 0.1, 6, 1, 'Normal', true, true)
        ring = table.concat(order, ' ')
    )"));
    CHECK(w.run("assert(area == 'second first along below', area)"));
    CHECK(w.run("assert(ring == area, ring)"));
}

TEST_CASE("A blast calls OnDamage on a destroyed unit and on one that can't take damage",
          "[damage]") {
    DamageWorld w;
    w.register_boxed_crate();
    REQUIRE(w.run(R"(
        guarded = CreateUnit('crate', 2, 10, 0, 10)
        guarded:SetCanTakeDamage(false)
        gone = CreateUnit('crate', 2, 11, 0, 10)
        moho.entity_methods.Destroy(gone)
        DamageArea(nil, {10.5, 0, 10}, 3, 1, 'Normal', true, true)
    )"));
    CHECK(w.run("assert(guarded.hits == 1, tostring(guarded.hits))"));
    CHECK(w.run("assert(gone.hits == 1, tostring(gone.hits))"));
}
