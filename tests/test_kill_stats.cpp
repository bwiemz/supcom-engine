#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

namespace {

int army_stat(lua_State* L) {
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, lua_upvalueindex(1)));
    const auto* brain = sim->get_army(static_cast<osc::i32>(lua_tonumber(L, 1)) - 1);
    lua_pushnumber(L, brain ? brain->get_stat(lua_tostring(L, 2), 0) : -1);
    return 1;
}

struct DeathWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit DeathWorld(bool under_water = false) {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(257) * 257, 1280);
        osc::map::Heightmap hm(256, 256, 1.0f / 128.0f, std::move(heights));
        const float water = under_water ? 20.0f : 0.0f;
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), water, under_water));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.add_army("ARMY_3", "ARMY_3");
        sim.set_victory_condition("sandbox");
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Economy = {BuildCostMass = 50},"
                 " Physics = {MotionType = 'RULEUMT_Land'}}",
                 "{BlueprintId = 'pgen', Categories = {'STRUCTURE'}, Defense = {MaxHealth = 100},"
                 " Economy = {BuildCostMass = 50}, Physics = {MotionType = 'RULEUMT_None'}}",
                 "{BlueprintId = 'lift', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Physics = {MotionType = 'RULEUMT_Air'}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        lua_pushlightuserdata(L, &sim);
        lua_pushcclosure(L, army_stat, 1);
        lua_setglobal(L, "stat");
        REQUIRE(run(R"(
            heard = {}
            Kept = setmetatable({
                OnKilled = function(self, instigator, type, ratio)
                    table.insert(heard, self.name .. ' killed')
                    self.by = instigator
                    self.type = type
                    self.ratio = ratio
                    self.lost_seen = stat(self:GetArmy(), 'Units_Killed')
                end,
                OnTransportDetach = function(self, bone, passenger)
                    table.insert(heard, self.name .. ' drops ' .. passenger.name)
                end,
                CheckCanBeKilled = function(self)
                    return self.killable ~= false
                end,
                SetDead = function(self)
                    table.insert(heard, self.name .. ' dead')
                end,
                OnAttachedKilled = function(self, attached)
                    table.insert(heard, self.name .. ': attached ' .. attached.name .. ' killed')
                    if self.lets_go then
                        attached:DetachFrom()
                    end
                end,
            }, {__index = function(_, k) return moho.unit_methods[k] or moho.entity_methods[k] end})
            Kept.__index = Kept
            placed = 0
            function make(bp, army, name)
                placed = placed + 1
                local u = CreateUnit(bp, army, 20 + 8 * placed, 10, 60)
                u.name = name
                return u
            end
            function heard_as_text()
                return table.concat(heard, ', ')
            end
        )"));
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"tank", "pgen", "lift"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Kept");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    bool run(const std::string& code) {
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

TEST_CASE("A kill is the instigator's army's if it is an enemy of the unit's, whoever did the "
          "most damage",
          "[kill][stats]") {
    const std::string by = GENERATE("third", "ally", "own", "nil");
    CAPTURE(by);
    DeathWorld w;
    if (by == "ally") {
        w.sim.get_army(1)->set_alliance(2, osc::sim::Alliance::Ally);
        w.sim.get_army(2)->set_alliance(1, osc::sim::Alliance::Ally);
    }
    REQUIRE(w.run(R"(
        first = make('tank', 1, 'first')
        third = make('tank', 3, 'third')
        ally = third
        own = make('tank', 2, 'own')
        victim = make('tank', 2, 'victim')
        Damage(first, victim, 90, 'Normal')
    )"));
    REQUIRE(w.run("victim:Kill(" + by + ", 'Normal', 0)"));
    const std::string third_kills = by == "third" ? "1" : "0";
    CHECK(w.run("assert(stat(2, 'Units_Killed') == 1, stat(2, 'Units_Killed'))"));
    CHECK(w.run("assert(stat(1, 'Enemies_Killed') == 0, stat(1, 'Enemies_Killed'))"));
    CHECK(w.run("assert(stat(2, 'Enemies_Killed') == 0, stat(2, 'Enemies_Killed'))"));
    CHECK(w.run("assert(stat(3, 'Enemies_Killed') == " + third_kills +
                ", stat(3, 'Enemies_Killed'))"));
}

TEST_CASE("A death is counted after the unit's OnKilled, and a Destroy without a kill is no "
          "loss",
          "[kill][stats]") {
    const std::string end = GENERATE("Kill", "Destroy", "Destroy after an enemy's damage");
    CAPTURE(end);
    DeathWorld w;
    REQUIRE(w.run("first = make('tank', 1, 'first') victim = make('tank', 2, 'victim')"));
    if (end == "Destroy after an enemy's damage") {
        REQUIRE(w.run("Damage(first, victim, 90, 'Normal')"));
    }
    REQUIRE(w.run(end == "Kill" ? "victim:Kill()" : "victim:Destroy()"));
    w.sim.run_deletion_queue();
    const bool killed = end == "Kill";
    if (killed) {
        CHECK(w.run("assert(victim.lost_seen == 0, victim.lost_seen)"));
    }
    CHECK(w.run(std::string("assert(stat(2, 'Units_Killed') == ") + (killed ? "1" : "0") +
                ", stat(2, 'Units_Killed'))"));
    CHECK(w.run("assert(stat(1, 'Enemies_Killed') == 0, stat(1, 'Enemies_Killed'))"));
}

TEST_CASE("Kill hands OnKilled its damage type and excess ratio as Moho's Kill does", "[kill]") {
    struct Row {
        const char* call;
        float built;
        const char* type;
        double ratio;
    };
    const Row row =
        GENERATE(Row{"victim:Kill()", 1.0f, "", 0}, Row{"victim:Kill(nil)", 1.0f, "", 0},
                 Row{"victim:Kill(first, 'Custom')", 1.0f, "Custom", 0},
                 Row{"victim:Kill(first, 'Custom', 2)", 1.0f, "Custom", 2},
                 Row{"victim:Kill(nil, 'Normal', 0.25)", 0.4f, "Normal", 10},
                 Row{"victim:Kill(nil, 'Normal', 0.25)", 0.6f, "Normal", 0.25});
    CAPTURE(row.call, row.built);
    DeathWorld w;
    REQUIRE(w.run("first = make('tank', 1, 'first') victim = make('pgen', 2, 'victim')"));
    if (row.built < 1.0f) {
        w.unit("victim")->set_is_being_built(true);
        w.unit("victim")->set_fraction_complete(row.built);
    }
    REQUIRE(w.run(row.call));
    CHECK(w.run(std::string("assert(victim.type == '") + row.type + "', victim.type)"));
    CHECK(w.run("assert(victim.ratio == " + std::to_string(row.ratio) + ", victim.ratio)"));
}

TEST_CASE("A transport's passengers die in its Kill after its SetDead, each let go first and "
          "with an excess ratio of 10, unless they cannot be killed",
          "[kill][transport]") {
    const bool killable = GENERATE(true, false);
    CAPTURE(killable);
    DeathWorld w;
    REQUIRE(w.run("t = make('lift', 1, 't') a = make('tank', 1, 'a') b = make('tank', 1, 'b')"));
    REQUIRE(w.run(killable ? "" : "a.killable = false b.killable = false"));
    osc::sim::Unit& lift = *w.unit("t");
    lift.set_transport_capacity(4);
    for (const char* name : {"a", "b"}) {
        w.unit(name)->attach_to_transport(&lift, w.sim.entity_registry(), w.state.raw());
    }
    REQUIRE(lift.cargo_ids().size() == 2);
    REQUIRE(w.run("t:Kill()"));
    const char* expected =
        killable ? "t dead, t drops a, a dead, a killed, t drops b, b dead, b killed, t killed"
                 : "t dead, t drops a, t drops b, t killed";
    CHECK(w.run(std::string("assert(heard_as_text() == '") + expected + "', heard_as_text())"));
    CHECK(lift.cargo_ids().empty());
    if (killable) {
        CHECK(w.run("assert(a.by == t and a.type == 'Damage' and a.ratio == 10, a.ratio)"));
    } else {
        CHECK_FALSE(w.unit("a")->is_dying());
        CHECK(w.unit("a")->transport_id() == 0);
    }
}

TEST_CASE("A transport killed where its passengers cannot stand keeps those its kill spares, "
          "unheard",
          "[kill][transport]") {
    const bool over_water = GENERATE(false, true);
    CAPTURE(over_water);
    DeathWorld w(over_water);
    REQUIRE(w.run("t = make('lift', 1, 't') a = make('tank', 1, 'a') b = make('tank', 1, 'b')"));
    REQUIRE(w.run("a.killable = false b.killable = false"));
    osc::sim::Unit& lift = *w.unit("t");
    lift.set_transport_capacity(4);
    lift.set_position({lift.position().x, 30.0f, lift.position().z});
    for (const char* name : {"a", "b"}) {
        w.unit(name)->attach_to_transport(&lift, w.sim.entity_registry(), w.state.raw());
    }
    REQUIRE(w.run("t:Kill()"));
    const char* expected =
        over_water ? "t dead, t killed" : "t dead, t drops a, t drops b, t killed";
    CHECK(w.run(std::string("assert(heard_as_text() == '") + expected + "', heard_as_text())"));
    CHECK(lift.cargo_ids().size() == (over_water ? 2U : 0U));
    CHECK(w.unit("a")->transport_id() == (over_water ? lift.entity_id() : 0U));
    CHECK(w.unit("a")->has_unit_state("Attached") == over_water);
    REQUIRE(w.run("t:Destroy()"));
    w.sim.run_deletion_queue();
    CHECK(w.run("assert(heard_as_text() == '" + std::string(expected) + "', heard_as_text())"));
    CHECK(w.unit("a")->transport_id() == 0U);
    CHECK_FALSE(w.unit("a")->has_unit_state("Attached"));
    CHECK(w.unit("a")->position().y < 15.0f);
}

TEST_CASE("A script's DetachFrom lets a carried unit go without the transport's "
          "OnTransportDetach, as Moho's Unit::DetachFrom",
          "[kill][transport]") {
    const bool killed = GENERATE(false, true);
    CAPTURE(killed);
    DeathWorld w(killed);
    REQUIRE(w.run("t = make('lift', 1, 't') a = make('tank', 1, 'a') b = make('tank', 1, 'b')"));
    REQUIRE(w.run("t.lets_go = true"));
    osc::sim::Unit& lift = *w.unit("t");
    lift.set_transport_capacity(4);
    for (const char* name : {"a", "b"}) {
        w.unit(name)->attach_to_transport(&lift, w.sim.entity_registry(), w.state.raw());
    }
    REQUIRE(w.run(killed ? "a:Kill()" : "a:DetachFrom()"));
    const char* expected = killed ? "a dead, t: attached a killed, a killed" : "";
    CHECK(w.run(std::string("assert(heard_as_text() == '") + expected + "', heard_as_text())"));
    CHECK(lift.cargo_ids().size() == 1U);
    CHECK(w.unit("a")->transport_id() == 0U);
    CHECK_FALSE(w.unit("a")->has_unit_state("Attached"));
    CHECK(w.unit("b")->transport_id() == lift.entity_id());
}

TEST_CASE("A unit killed in its carrier's storage leaves it unheard, but stays in an aircraft's "
          "storage where it cannot stand",
          "[kill][transport]") {
    const bool aircraft = GENERATE(false, true);
    CAPTURE(aircraft);
    DeathWorld w(true);
    REQUIRE(w.run(std::string("c = make('") + (aircraft ? "lift" : "pgen") +
                  "', 1, 'c') a = make('tank', 1, 'a')"));
    osc::sim::Unit& carrier = *w.unit("c");
    osc::sim::Unit& stored = *w.unit("a");
    carrier.set_storage_slots(2);
    carrier.add_to_storage(stored, w.sim.entity_registry(), w.state.raw());
    REQUIRE(carrier.is_stored_unit(stored.entity_id()));
    REQUIRE(w.run("a:Kill()"));
    const char* expected = aircraft ? "a dead, c: attached a killed, a killed" : "a dead, a killed";
    CHECK(w.run(std::string("assert(heard_as_text() == '") + expected + "', heard_as_text())"));
    CHECK(w.run("assert(a.ratio == 10, a.ratio)"));
    CHECK(carrier.is_stored_unit(stored.entity_id()) == aircraft);
    CHECK(stored.transport_id() == (aircraft ? carrier.entity_id() : 0U));
}
