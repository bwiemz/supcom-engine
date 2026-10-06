#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/input_handler.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <utility>

namespace {

struct KillWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    KillWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'tank', Categories = {'LAND'}, Defense = {MaxHealth = 100},"
                 " Economy = {BuildCostMass = 56},"
                 " Veteran = {Level1 = 3, Level2 = 6, Level3 = 9, Level4 = 12, Level5 = 15}}",
                 "{BlueprintId = 'scripted', Categories = {'LAND'}, Defense = {MaxHealth = 100},"
                 " Economy = {BuildCostMass = 56}}",
                 "{BlueprintId = 'wall', Categories = {'BENIGN'}, Defense = {MaxHealth = 100},"
                 " Economy = {BuildCostMass = 56}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string(R"(
            Plain = setmetatable({}, {__index = moho.unit_methods})
            Plain.__index = Plain
            Scripted = setmetatable({
                OnKilled = function(self, instigator)
                    seen_kills = instigator:GetStat('KILLS', 0).Value
                    moho.entity_methods.Destroy(self)
                end,
            }, {__index = moho.unit_methods})
            Scripted.__index = Scripted
        )")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const auto& [id, cls] : {std::pair{"tank", "Plain"}, std::pair{"wall", "Plain"},
                                      std::pair{"scripted", "Scripted"}}) {
            lua_pushstring(L, id);
            lua_getglobal(L, cls);
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

TEST_CASE("Kill counts an enemy in the instigator's KILLS after its OnKilled", "[veterancy]") {
    KillWorld w;
    CHECK(w.check(R"(
        killer = CreateUnit('tank', 1, 0, 0, 0)
        local victim = CreateUnit('scripted', 2, 5, 0, 0)
        Damage(killer, victim, 10, 'Normal')
        victim:Kill(killer, 'Normal', 0)
        assert(killer:GetVeterancyLevel() == 0, 'level ' .. killer:GetVeterancyLevel())
        assert(seen_kills == 0, 'OnKilled saw ' .. tostring(seen_kills))
        assert(killer:GetStat('KILLS', 0).Value == 1, 'kills ' .. killer:GetStat('KILLS', 0).Value)
        CreateUnit('tank', 2, 5, 0, 0):Kill(killer)
        assert(killer:GetStat('KILLS', 0).Value == 2, 'kills ' .. killer:GetStat('KILLS', 0).Value)
    )"));
}

TEST_CASE("Kill does not count a BENIGN, friendly or unfinished unit, or no instigator",
          "[veterancy]") {
    KillWorld w;
    REQUIRE(w.check(R"(
        killer = CreateUnit('tank', 1, 0, 0, 0)
        unfinished = CreateUnit('tank', 2, 5, 0, 0)
    )"));
    w.unit("unfinished")->set_is_being_built(true);
    CHECK(w.check(R"(
        unfinished:Kill(killer)
        local wall = CreateUnit('wall', 2, 5, 0, 0)
        Damage(killer, wall, 10, 'Normal')
        wall:Kill(killer)
        CreateUnit('tank', 1, 5, 0, 0):Kill(killer)
        CreateUnit('tank', 2, 5, 0, 0):Kill()
        assert(killer:GetStat('KILLS', 0).Value == 0, 'kills ' .. killer:GetStat('KILLS', 0).Value)
        assert(killer:GetVeterancyLevel() == 0, 'level ' .. killer:GetVeterancyLevel())
    )"));
}

TEST_CASE("GetRolloverInfo reports the unit's KILLS", "[veterancy][ui]") {
    KillWorld w;
    osc::lua::register_user_bindings(w.state);
    osc::renderer::InputHandler input;
    lua_State* L = w.state.raw();
    lua_pushstring(L, "__osc_input_handler");
    lua_pushlightuserdata(L, &input);
    lua_rawset(L, LUA_REGISTRYINDEX);
    REQUIRE(w.check("killer = CreateUnit('tank', 1, 0, 0, 0) killer:SetStat('KILLS', 4)"));
    input.set_selected({w.unit("killer")->entity_id()});
    CHECK(w.check("assert(GetRolloverInfo().kills == 4, 'kills ' .. GetRolloverInfo().kills)"));
}
