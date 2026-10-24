#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

struct CallbackWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    CallbackWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.entity_registry().init_spatial_grid(512, 512);
        lua_State* L = state.raw();
        REQUIRE(state
                    .do_string("return {BlueprintId = 'tank', Categories = {'LAND'},"
                               " Defense = {MaxHealth = 1000}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string(R"(
            heard = {}
            vectors = {}
            Tank = setmetatable({
                OnDamageBy = function(self, army)
                    table.insert(heard, self.name .. ' by ' .. army)
                end,
                OnDamage = function(self, instigator, amount, vector)
                    table.insert(heard, self.name .. ' ' .. amount)
                    vectors[self.name] = vector
                end,
            }, {__index = moho.unit_methods})
            Tank.__index = Tank
            ShieldClass = setmetatable({
                OnGetDamageAbsorption = function(self, instigator, amount)
                    return math.min(self:GetHealth(), amount)
                end,
                OnDamage = function(self, instigator, amount)
                    table.insert(heard, 'shield ' .. amount)
                end,
            }, {__index = function(_, k) return moho.shield_methods[k] or moho.entity_methods[k] end})
            ShieldClass.__index = ShieldClass
            function tank(name, army, x, y)
                local u = CreateUnit('tank', army, x, y or 0, 100)
                u.name = name
                return u
            end
            function heard_as_text()
                return table.concat(heard, ', ')
            end
        )")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "tank");
        lua_getglobal(L, "Tank");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    bool check(const std::string& code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }
};

} // namespace

TEST_CASE("A unit hears OnDamageBy with its attacker's army before OnDamage", "[damage]") {
    const int army = GENERATE(1, 2);
    const bool area = GENERATE(false, true);
    CAPTURE(army, area);
    CallbackWorld w;
    REQUIRE(w.check("attacker = tank('attacker', " + std::to_string(army) +
                    ", 90) victim = tank('victim', 2, 100)"));
    REQUIRE(w.check(area ? "DamageArea(attacker, {100, 0, 100}, 2, 10, 'Normal', true)"
                         : "Damage(attacker, {90, 0, 100}, victim, 10, 'Normal')"));
    const std::string expected = "victim by " + std::to_string(army) + ", victim 10";
    CHECK(w.check("assert(heard_as_text() == '" + expected + "', heard_as_text())"));
}

TEST_CASE("A blast reaches the units before the shield it meets and the shield takes what it "
          "absorbs",
          "[damage]") {
    const bool ring = GENERATE(false, true);
    CAPTURE(ring);
    CallbackWorld w;
    REQUIRE(w.check(R"(
        attacker = tank('attacker', 1, 150)
        local gen = tank('gen', 2, 100)
        shield = setmetatable({}, ShieldClass)
        _c_CreateShield(shield, {Owner = gen, Size = 20, ShieldMaxHealth = 1000})
        shield:SetCollisionShape('Sphere', 0, 0, 0, 10)
        shield:SetHealth(shield, 30)
        tank('outside', 2, 114)
    )"));
    REQUIRE(w.check(ring ? "DamageRing(attacker, {112, 0, 100}, 0.1, 5, 100, 'Normal', false)"
                         : "DamageArea(attacker, {112, 0, 100}, 5, 100, 'Normal', false)"));
    const char* expected =
        ring ? "outside by 1, outside 100" : "outside by 1, outside 100, shield 30";
    CHECK(w.check(std::string("assert(heard_as_text() == '") + expected + "', heard_as_text())"));
}

TEST_CASE("A blast's vector runs from its centre to what it hits, height and all", "[damage]") {
    const bool ring = GENERATE(false, true);
    CAPTURE(ring);
    CallbackWorld w;
    REQUIRE(w.check("attacker = tank('attacker', 1, 150) victim = tank('victim', 2, 103, 1)"));
    REQUIRE(w.check(ring ? "DamageRing(attacker, {100, 5, 100}, 0.1, 8, 10, 'Normal', false)"
                         : "DamageArea(attacker, {100, 5, 100}, 8, 10, 'Normal', false)"));
    CHECK(w.check(R"(
        local v = vectors.victim
        assert(v, 'no vector: ' .. heard_as_text())
        local p = moho.entity_methods.GetPosition(victim)
        local want = {p[1] - 100, p[2] - 5, p[3] - 100}
        for i = 1, 3 do
            assert(math.abs(v[i] - want[i]) < 1e-4,
                string.format('%g,%g,%g, not %g,%g,%g', v[1], v[2], v[3], want[1], want[2], want[3]))
        end
    )"));
}
