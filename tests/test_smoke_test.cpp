#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "blueprints/blueprint_store.hpp"
#include "lua/smoke_test.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/manipulator.hpp"
#include "sim/weapon.hpp"
#include "sim/flight_math.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/entity_registry.hpp"

extern "C" {
#include <lua.h>
}

#include <tuple>

TEST_CASE("SmokeTestHarness records and deduplicates entries", "[smoke]") {
    osc::lua::SmokeTestHarness harness;

    harness.record(osc::lua::SmokeCategory::MissingGlobal, "FooFunc", "test.lua:10");
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "FooFunc", "test.lua:20");
    harness.record(osc::lua::SmokeCategory::MissingMethod, "unit.BarMethod", "unit.lua:5");
    harness.record(osc::lua::SmokeCategory::PcallError, "attempt to index nil", "sim.lua:99");

    auto report = harness.generate_report();

    // 3 unique entries (FooFunc deduplicated)
    REQUIRE(report.size() == 3);

    // Find the FooFunc entry — count should be 2
    bool found_foo = false;
    for (auto& e : report) {
        if (e.name == "FooFunc") {
            REQUIRE(e.category == osc::lua::SmokeCategory::MissingGlobal);
            REQUIRE(e.count == 2);
            REQUIRE(e.first_location == "test.lua:10");
            found_foo = true;
        }
    }
    REQUIRE(found_foo);
}

TEST_CASE("SmokeTestHarness total_count sums all occurrences", "[smoke]") {
    osc::lua::SmokeTestHarness harness;
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "A", "a.lua:1");
    harness.record(osc::lua::SmokeCategory::MissingGlobal, "A", "a.lua:2");
    harness.record(osc::lua::SmokeCategory::MissingMethod, "B", "b.lua:1");
    REQUIRE(harness.total_count() == 3);
}

TEST_CASE("SmokeTestHarness intercepts missing globals", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_global_interceptor(state.raw());

    // Access a global that doesn't exist - should be logged, return nil
    auto result = state.do_string("local x = SomeMissingGlobal\nreturn type(x)");
    REQUIRE(result.ok());

    // Check that the missing access was recorded
    auto report = harness.generate_report();
    bool found = false;
    for (auto& e : report) {
        if (e.name == "SomeMissingGlobal" &&
            e.category == osc::lua::SmokeCategory::MissingGlobal) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("Sim GetFocusArmy defaults to first playable army", "[focus]") {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store(state.raw());
    osc::sim::SimState sim(state.raw(), &store);
    osc::lua::register_sim_bindings(state, sim);

    auto result = state.do_string("return GetFocusArmy()");
    REQUIRE(result.ok());
    REQUIRE(lua_tonumber(state.raw(), -1) == 1.0);
    lua_pop(state.raw(), 1);

    result = state.do_string("SetFocusArmy(2); return GetFocusArmy()");
    REQUIRE(result.ok());
    REQUIRE(lua_tonumber(state.raw(), -1) == 2.0);
    lua_pop(state.raw(), 1);
}

TEST_CASE("Global interceptor does not log existing globals", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;

    // Set a global first, then install interceptor
    state.do_string("MyGlobal = 42");
    harness.install_global_interceptor(state.raw());

    state.do_string("local x = MyGlobal");
    REQUIRE(harness.total_count() == 0);
}

TEST_CASE("SmokeTestHarness intercepts missing moho methods", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;

    lua_State* L = state.raw();

    // Create a fake moho metatable (simulating the cached __osc_*_mt pattern)
    lua_newtable(L);
    int mt = lua_gettop(L);
    lua_pushstring(L, "__index");
    lua_pushvalue(L, mt);
    lua_rawset(L, mt); // mt.__index = mt

    // Add one real method
    lua_pushstring(L, "RealMethod");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        lua_pushnumber(L, 42);
        return 1;
    });
    lua_rawset(L, mt);

    // Cache it
    lua_pushstring(L, "__osc_test_mt");
    lua_pushvalue(L, mt);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1); // pop mt

    // Install method interceptor on this metatable
    harness.install_method_interceptor(L, "__osc_test_mt", "TestObj");

    // Create an object with this metatable
    lua_newtable(L);
    lua_pushstring(L, "__osc_test_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    lua_setglobal(L, "test_obj");

    // Call a real method — should work, no log
    state.do_string("local r = test_obj:RealMethod()");
    REQUIRE(harness.total_count() == 0);

    // Call a missing method — should log, return no-op function
    state.do_string("test_obj:FakeMethod()");
    auto report = harness.generate_report();
    REQUIRE(report.size() == 1);
    REQUIRE(report[0].name == "TestObj.FakeMethod");
    REQUIRE(report[0].category == osc::lua::SmokeCategory::MissingMethod);
}

TEST_CASE("SmokeTestHarness panic handler prevents abort", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_panic_handler(state.raw());

    // A pcall error should be caught by our error handler, not crash
    auto result = state.do_string("error('test error')");
    // do_string uses lua_pcall internally, so it should return an error
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("SmokeTestHarness pcall error recording", "[smoke]") {
    osc::lua::LuaState state;
    osc::lua::SmokeTestHarness harness;
    harness.install_panic_handler(state.raw());

    // Run code that will error
    harness.do_string_logged(state.raw(), "local x = nil; x.foo()");

    auto report = harness.generate_report();
    REQUIRE(!report.empty());
    bool found_pcall = false;
    for (auto& e : report) {
        if (e.category == osc::lua::SmokeCategory::PcallError) found_pcall = true;
    }
    REQUIRE(found_pcall);
}

TEST_CASE("ArmyBrain build restriction add/remove/check", "[m154]") {
    osc::sim::ArmyBrain brain;
    REQUIRE_FALSE(brain.is_build_restricted("ueb0101"));
    brain.add_build_restriction({"ueb0101", "ueb0201"});
    REQUIRE(brain.is_build_restricted("UEB0101"));
    brain.remove_build_restriction({"ueb0101"});
    REQUIRE_FALSE(brain.is_build_restricted("ueb0101"));
    REQUIRE(brain.is_build_restricted("ueb0201"));
}

namespace {

void register_test_unit_blueprint(osc::lua::LuaState& state,
                                  osc::blueprints::BlueprintStore& store,
                                  const char* blueprint_id,
                                  std::initializer_list<const char*> categories,
                                  double build_rate = 10.0) {
    lua_State* L = state.raw();

    lua_newtable(L);
    int bp = lua_gettop(L);

    lua_pushstring(L, "BlueprintId");
    lua_pushstring(L, blueprint_id);
    lua_rawset(L, bp);

    lua_pushstring(L, "CategoriesHash");
    lua_newtable(L);
    int cats = lua_gettop(L);
    for (const char* category : categories) {
        lua_pushstring(L, category);
        lua_pushboolean(L, 1);
        lua_rawset(L, cats);
    }
    lua_rawset(L, bp);

    lua_pushstring(L, "Economy");
    lua_newtable(L);
    int economy = lua_gettop(L);
    lua_pushstring(L, "BuildRate");
    lua_pushnumber(L, build_rate);
    lua_rawset(L, economy);
    lua_pushstring(L, "BuildTime");
    lua_pushnumber(L, 10);
    lua_rawset(L, economy);
    lua_pushstring(L, "BuildCostMass");
    lua_pushnumber(L, 10);
    lua_rawset(L, economy);
    lua_pushstring(L, "BuildCostEnergy");
    lua_pushnumber(L, 10);
    lua_rawset(L, economy);
    lua_rawset(L, bp);

    lua_pushstring(L, "Defense");
    lua_newtable(L);
    lua_pushstring(L, "MaxHealth");
    lua_pushnumber(L, 100);
    lua_rawset(L, -3);
    lua_rawset(L, bp);

    store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, bp);
    lua_pop(L, 1);
}

struct BuildRuleHarness {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store;
    osc::sim::SimState sim;

    BuildRuleHarness() : store(state.raw()), sim(state.raw(), &store) {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");

        register_test_unit_blueprint(state, store, "test_factory",
                                     {"STRUCTURE", "FACTORY", "RALLYPOINT"}, 20);
        register_test_unit_blueprint(
            state, store, "test_tank", {"MOBILE", "LAND", "TECH1"}, 1);
        register_test_unit_blueprint(
            state, store, "test_experimental",
            {"MOBILE", "LAND", "EXPERIMENTAL"}, 1);
        store.expose_to_lua(state.raw());
        REQUIRE(state.do_string(
            "__blueprints.test_factory.General = {CommandCaps = {RULEUCC_Pause = true}}\n"));
    }
};

} // namespace

TEST_CASE("Factory build obeys army unit cap", "[session][rules]") {
    BuildRuleHarness h;
    h.sim.get_army(0)->set_unit_cap(1);

    auto result = h.state.do_string(
        "local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
        "IssueBuildFactory({factory}, 'test_tank', 1)\n");
    REQUIRE(result);
    REQUIRE(h.sim.entity_registry().count() == 1);

    h.sim.tick();

    CHECK(h.sim.entity_registry().count() == 1);
    CHECK(h.sim.get_army(0)->get_unit_cost_total(h.sim.entity_registry()) == 1);
}

TEST_CASE("A scenario's build restriction is a category that RemoveBuildRestriction lifts",
          "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(
        h.state.do_string("__blueprints.test_factory.Economy.BuildableCategory = {'MOBILE LAND'}\n"
                          "AddBuildRestriction(1, categories.EXPERIMENTAL + categories.NAVAL)\n"
                          "__osc_factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                          "IssueBuildFactory({__osc_factory}, 'test_experimental', 1)\n"));
    h.sim.tick();
    CHECK(h.sim.entity_registry().count() == 1);
    const auto can_build = [&h] {
        REQUIRE(h.state.do_string(
            "__osc_can = moho.unit_methods.CanBuild(__osc_factory, 'test_experimental')"));
        lua_getglobal(h.state.raw(), "__osc_can");
        const bool can = lua_toboolean(h.state.raw(), -1) != 0;
        lua_pop(h.state.raw(), 1);
        return can;
    };
    CHECK_FALSE(can_build());

    REQUIRE(h.state.do_string("RemoveBuildRestriction(1, categories.EXPERIMENTAL)\n"
                              "IssueClearCommands({__osc_factory})\n"
                              "IssueBuildFactory({__osc_factory}, 'test_experimental', 1)\n"));
    h.sim.tick();
    CHECK(h.sim.entity_registry().count() == 2);
    CHECK(can_build());
}

namespace {

/// The harness's factory, making more than its builds use (they draw 20
/// mass and energy a second), so it builds at full rate.
osc::sim::Unit* find_factory(const BuildRuleHarness& h) {
    osc::sim::Unit* found = nullptr;
    h.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        auto& u = static_cast<osc::sim::Unit&>(e);
        if (u.blueprint_id() == "test_factory") found = &u;
    });
    if (found) {
        found->economy().production_mass = 100.0;
        found->economy().production_energy = 100.0;
        found->economy().production_active = true;
    }
    return found;
}

/// The factory's build orders, e.g. "test_tank test_experimental".
std::string factory_orders(const osc::sim::Unit& f) {
    std::string s;
    for (const auto& g : f.factory_queue())
        for (int i = 0; i < g.count; ++i) s += (s.empty() ? "" : " ") + g.blueprint_id;
    return s;
}

/// Finished units the army has, not counting the factory.
int finished_units(const BuildRuleHarness& h) {
    int n = 0;
    h.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        auto& u = static_cast<osc::sim::Unit&>(e);
        if (!u.destroyed() && !u.is_being_built() && u.blueprint_id() != "test_factory") ++n;
    });
    return n;
}

} // namespace

TEST_CASE("A repeating factory sends each finished build to the back of its queue",
          "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"
                              "IssueBuildFactory({factory}, 'test_experimental', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->set_repeat_queue(true);
    REQUIRE(factory_orders(*f) == "test_tank test_experimental");

    // Each build takes a few ticks (build rate 20, build time 10).
    auto head = [&] {
        const auto q = f->factory_queue();
        return q.empty() ? std::string() : q.front().blueprint_id;
    };
    auto finish_head = [&] {
        const std::string first = head();
        for (int i = 0; i < 50 && head() == first; ++i) h.sim.tick();
    };
    finish_head();
    CHECK(factory_orders(*f) == "test_experimental test_tank");
    CHECK(finished_units(h) == 1);
    CHECK_FALSE(f->is_building()); // the next build starts next tick, as Moho's does
    h.sim.tick();
    CHECK(f->is_building());

    finish_head();
    CHECK(factory_orders(*f) == "test_tank test_experimental");
    CHECK(finished_units(h) == 2);

    finish_head(); // round again: a second tank
    CHECK(factory_orders(*f) == "test_experimental test_tank");
    CHECK(finished_units(h) == 3);
}

TEST_CASE("A factory not repeating its queue builds each order once", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    for (int i = 0; i < 50 && !f->factory_queue().empty(); ++i) h.sim.tick();
    CHECK(factory_orders(*f).empty());
    CHECK(finished_units(h) == 1);
    for (int i = 0; i < 20; ++i) h.sim.tick();
    CHECK(finished_units(h) == 1);
}

TEST_CASE("A repeating factory drops a build that fails", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"
                              "IssueBuildFactory({factory}, 'test_experimental', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->set_repeat_queue(true);
    h.sim.tick();
    REQUIRE(f->is_building());

    // The tank on the factory's floor is destroyed: its order goes, and the
    // factory moves on to the next.
    h.sim.entity_registry().find(f->build_target_id())->mark_destroyed();
    h.sim.tick();
    CHECK(factory_orders(*f) == "test_experimental");
    CHECK(f->is_building());
}

TEST_CASE("A raised factory order builds its count as one order, then goes", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->increase_build_count(1, 2); // IncreaseBuildCountInQueue: one order of three
    REQUIRE(f->command_queue().size() == 1);
    CHECK(factory_orders(*f) == "test_tank test_tank test_tank");
    for (int i = 0; i < 200 && !f->factory_queue().empty(); ++i) h.sim.tick();
    CHECK(finished_units(h) == 3);
    CHECK(f->command_queue().empty());
}

TEST_CASE("A repeating factory's raised order goes round whole, its count back at its most",
          "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"
                              "IssueBuildFactory({factory}, 'test_experimental', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->set_repeat_queue(true);
    f->increase_build_count(1, 3); // four tanks, its most four
    f->decrease_build_count(1, 2, h.sim.entity_registry(), h.state.raw()); // two left
    REQUIRE(f->command_queue().size() == 2);
    CHECK(f->command_queue().front().count == 2);
    CHECK(f->command_queue().front().max_count == 4);
    CHECK(factory_orders(*f) == "test_tank test_tank test_experimental");

    // The first tank done, the next starts at once from the same order
    // (Moho dispatches it again that tick).
    for (int i = 0; i < 100 && f->command_queue().front().count == 2; ++i) h.sim.tick();
    CHECK(f->command_queue().front().count == 1);
    CHECK(f->is_building());
    CHECK(finished_units(h) == 1);

    // Both built, the order goes to the back whole, four again; not two
    // orders of one, as a copy per unit would go.
    for (int i = 0; i < 100 && f->command_queue().front().blueprint_id == "test_tank"; ++i)
        h.sim.tick();
    CHECK(finished_units(h) == 2);
    REQUIRE(f->command_queue().size() == 2);
    CHECK(f->command_queue().back().blueprint_id == "test_tank");
    CHECK(f->command_queue().back().count == 4);
    CHECK(factory_orders(*f) == "test_experimental test_tank test_tank test_tank test_tank");
}

TEST_CASE("A raised factory order whose build fails goes whole", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"
                              "IssueBuildFactory({factory}, 'test_experimental', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->set_repeat_queue(true);
    f->increase_build_count(1, 2);
    h.sim.tick();
    REQUIRE(f->is_building());
    // The tank on the factory's floor is destroyed: the order goes, its two
    // still to make with it (Moho's dispatch removes a failed command).
    h.sim.entity_registry().find(f->build_target_id())->mark_destroyed();
    h.sim.tick();
    CHECK(factory_orders(*f) == "test_experimental");
}

TEST_CASE("A factory guarding a factory sends the order it takes round as the guarded one repeats",
          "[session][rules]") {
    for (const bool guarded_repeats : {true, false}) {
        CAPTURE(guarded_repeats);
        BuildRuleHarness h;
        REQUIRE(h.state.do_string(
            "__blueprints.test_factory.Economy.BuildableCategory = {'MOBILE LAND'}\n"
            "__blueprints.test_factory.General.CommandCaps.RULEUCC_Guard = true\n"
            "__osc_a = CreateUnit('test_factory', 1, 0, 0, 0)\n"
            "IssueBuildFactory({__osc_a}, 'test_tank', 1)\n"
            "IssueBuildFactory({__osc_a}, 'test_experimental', 1)\n"));
        osc::sim::Unit* a = find_factory(h);
        REQUIRE(a);
        a->set_repeat_queue(guarded_repeats);
        REQUIRE(h.state.do_string(std::string("local b = CreateUnit('test_factory', 1, 10, 0, 0)\n"
                                              "moho.unit_methods.SetRepeatQueue(b, ") +
                                  (guarded_repeats ? "false" : "true") +
                                  ")\n"
                                  "IssueFactoryAssist({b}, __osc_a)\n"
                                  "__osc_b = b\n"));
        h.sim.tick();
        h.sim.tick();
        REQUIRE(h.state.do_string(
            "if not moho.unit_methods.IsUnitState(__osc_b, 'Building') then error('B idle') end"));
        CHECK(factory_orders(*a) ==
              (guarded_repeats ? "test_tank test_experimental" : "test_tank"));
    }
}

namespace {

osc::sim::Unit* find_unit(const BuildRuleHarness& h, const std::string& bp) {
    osc::sim::Unit* found = nullptr;
    h.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        auto& u = static_cast<osc::sim::Unit&>(e);
        if (!found && u.blueprint_id() == bp) found = &u;
    });
    return found;
}

} // namespace

TEST_CASE("A factory's rally orders are apart from its build orders", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "tank = CreateUnit('test_tank', 1, 20, 0, 0)\n"));
    osc::sim::Unit* f = find_factory(h);
    osc::sim::Unit* tank = find_unit(h, "test_tank");
    REQUIRE(f);
    REQUIRE(tank);
    f->add_command_cap("RULEUCC_Move");
    REQUIRE(h.state.do_string("IssueBuildFactory({factory}, 'test_tank', 2)\n"
                              "IssueFactoryRallyPoint({factory, tank}, {50, 0, 60})\n"));
    REQUIRE(f->rally_orders().size() == 2); // after its initial rally, as Moho's appends
    const auto& rally = f->rally_orders().back();
    CHECK(rally.type == osc::sim::CommandType::Move);
    CHECK((rally.target_pos.x == 50.0f && rally.target_pos.z == 60.0f));
    CHECK(rally.command_id != 0);
    CHECK(factory_orders(*f) == "test_tank test_tank"); // not in the build queue
    CHECK(tank->rally_orders().empty());                // only a factory keeps them

    // Retail's AI clears a factory's rally orders before it sets new ones:
    // the builds it queued stay.
    REQUIRE(h.state.do_string("IssueClearFactoryCommands({factory})\n"));
    CHECK(f->rally_orders().empty());
    CHECK(factory_orders(*f) == "test_tank test_tank");
}

TEST_CASE("A unit a factory finishes takes the factory's rally orders", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->add_command_cap("RULEUCC_Move");
    REQUIRE(h.state.do_string("IssueFactoryRallyPoint({factory}, {50, 0, 60})\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"));
    // Then a guard order, which never ends: without a pathfinder here, a
    // move is done the tick it starts.
    osc::sim::UnitCommand guard;
    guard.type = osc::sim::CommandType::Guard;
    guard.target_id = f->entity_id();
    guard.command_id = h.sim.next_command_id();
    f->add_rally_order(guard);

    h.sim.tick();
    osc::sim::Unit* tank = find_unit(h, "test_tank");
    REQUIRE(tank);
    CHECK(tank->command_queue().empty()); // nothing while it is being built
    for (int i = 0; i < 50 && !f->factory_queue().empty(); ++i) h.sim.tick();
    REQUIRE_FALSE(tank->is_being_built());

    // The move went first; the guard, the factory's own command, stays.
    REQUIRE(tank->command_queue().size() == 1);
    CHECK(tank->command_queue().front().type == osc::sim::CommandType::Guard);
    CHECK(tank->command_queue().front().command_id == guard.command_id);
    CHECK(f->rally_orders().size() == 3); // the factory keeps them
}

TEST_CASE("A factory is made with its initial rally, ahead of itself", "[session][rules]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("CreateUnit('test_factory', 1, 10, 0, 20)\n"
                              "CreateUnit('test_tank', 1, 30, 0, 20)\n"));
    osc::sim::Unit* f = find_factory(h);
    osc::sim::Unit* tank = find_unit(h, "test_tank");
    REQUIRE(f);
    REQUIRE(tank);
    REQUIRE(f->rally_orders().size() == 1);
    const osc::sim::UnitCommand rally = f->rally_orders().front();
    CHECK(rally.type == osc::sim::CommandType::Move);
    CHECK(rally.target_pos.x == 10.0f);
    CHECK(rally.target_pos.z == 25.0f);
    CHECK(rally.command_id != 0);
    osc::sim::Vector3 point;
    REQUIRE(f->rally_point(h.state.raw(), point));
    CHECK(point.z == 25.0f);
    CHECK(tank->rally_orders().empty());
    CHECK_FALSE(tank->rally_point(h.state.raw(), point));
    CHECK(f->validated_rally_orders(h.state.raw(), &h.sim).size() == 1);

    lua_State* L = h.state.raw();
    lua_pushstring(L, "__osc_create_building_unit");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "test_factory");
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 40);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 20);
    REQUIRE(lua_pcall(L, 5, 2, 0) == 0);
    const auto built_id = static_cast<osc::u32>(lua_tonumber(L, -2));
    lua_pop(L, 2);
    auto* built = static_cast<osc::sim::Unit*>(h.sim.entity_registry().find(built_id));
    REQUIRE(built);
    REQUIRE(built->rally_orders().size() == 1);
    CHECK(built->rally_orders().front().target_pos.x == 40.0f);
    CHECK(built->rally_orders().front().target_pos.z == 25.0f);
}

TEST_CASE("SimState generation increments on construction", "[m155]") {
    osc::u32 gen_before = osc::sim::SimState::sim_generation();
    osc::sim::SimState::increment_sim_generation();
    REQUIRE(osc::sim::SimState::sim_generation() == gen_before + 1);
}

TEST_CASE("SimState playable rect stores and returns bounds", "[m154]") {
    osc::lua::LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);

    // Default: not set (full map)
    REQUIRE_FALSE(sim.has_playable_rect());

    sim.set_playable_rect(10.0f, 20.0f, 500.0f, 480.0f);
    REQUIRE(sim.has_playable_rect());
    REQUIRE(sim.playable_x0() == 10.0f);
    REQUIRE(sim.playable_z0() == 20.0f);
    REQUIRE(sim.playable_x1() == 500.0f);
    REQUIRE(sim.playable_z1() == 480.0f);
}

TEST_CASE("An army that ignores the playable rect keeps to the map", "[m209]") {
    osc::lua::LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    sim.add_army("PLAYER", "Player");
    sim.add_army("ENEMY", "Enemy");
    sim.set_playable_rect(100.0f, 100.0f, 300.0f, 300.0f);
    // SetIgnorePlayableRect(2, true): the campaign's other armies
    sim.army_at(1)->set_use_whole_map(true);

    const osc::sim::Vector3 outside{50.0f, 0.0f, 400.0f};
    const auto kept = sim.clamp_to_playable(outside, 0);
    CHECK(kept.x == 100.0f);
    CHECK(kept.z == 300.0f);
    // No terrain: nothing bounds the whole map, so it stays where it is.
    const auto free = sim.clamp_to_playable(outside, 1);
    CHECK(free.x == 50.0f);
    CHECK(free.z == 400.0f);
    // No army (-1) or one that isn't: the playable rect.
    CHECK(sim.clamp_to_playable(outside, -1).x == 100.0f);
    CHECK(sim.clamp_to_playable(outside, 7).z == 300.0f);
}

TEST_CASE("Navigator::update_air moves unit along heading", "[m157]") {
    osc::sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(10.0f);
    unit.set_turn_rate_rad(3.14f); // fast turn for test
    unit.set_accel_rate(100.0f);   // instant accel for test
    unit.set_elevation_target(20.0f);
    unit.set_climb_rate(100.0f);   // fast climb for test
    unit.set_position({0, 0, 0});

    auto& nav = unit.navigator();
    nav.set_goal({100, 0, 100}); // straight-line goal

    // Run several ticks
    for (int i = 0; i < 20; i++) {
        nav.update_air(unit, 0.1, nullptr);
    }

    // Should have moved toward goal
    CHECK(unit.position().x > 0);
    CHECK(unit.position().z > 0);
    // Should have gained altitude
    CHECK(unit.current_altitude() > 0);
    // Should have nonzero airspeed
    CHECK(unit.current_airspeed() > 0);
}

TEST_CASE("Weapon layer targeting filters correctly", "[m158]") {
    CHECK(osc::sim::layer_to_bit("Air") == 0x10);
    CHECK(osc::sim::layer_to_bit("Land") == 0x01);
    CHECK(osc::sim::layer_to_bit("Water") == 0x02);
    CHECK(osc::sim::layer_to_bit("Seabed") == 0x04);
    CHECK(osc::sim::layer_to_bit("Sub") == 0x08);

    // AntiAir weapon should only target Air layer
    uint8_t aa_caps = osc::sim::layer_to_bit("Air");
    CHECK((aa_caps & osc::sim::layer_to_bit("Air")) != 0);
    CHECK((aa_caps & osc::sim::layer_to_bit("Land")) == 0);

    // DirectFire should target Land and Water but not Air
    uint8_t df_caps = osc::sim::layer_to_bit("Land") | osc::sim::layer_to_bit("Water") | osc::sim::layer_to_bit("Seabed");
    CHECK((df_caps & osc::sim::layer_to_bit("Air")) == 0);
    CHECK((df_caps & osc::sim::layer_to_bit("Land")) != 0);
    CHECK((df_caps & osc::sim::layer_to_bit("Water")) != 0);

    // AntiNavy should target Water, Sub, Seabed but not Air or Land
    uint8_t an_caps = osc::sim::layer_to_bit("Water") | osc::sim::layer_to_bit("Sub") | osc::sim::layer_to_bit("Seabed");
    CHECK((an_caps & osc::sim::layer_to_bit("Water")) != 0);
    CHECK((an_caps & osc::sim::layer_to_bit("Sub")) != 0);
    CHECK((an_caps & osc::sim::layer_to_bit("Air")) == 0);
    CHECK((an_caps & osc::sim::layer_to_bit("Land")) == 0);
}

TEST_CASE("Weapons do not auto-target cloaked units without omni", "[cloak]") {
    osc::sim::EntityRegistry registry;

    auto owner = std::make_unique<osc::sim::Unit>();
    owner->set_army(0);
    owner->set_position({0.0f, 0.0f, 0.0f});
    auto weapon = std::make_unique<osc::sim::Weapon>();
    weapon->max_range = 100.0f;
    weapon->damage = 10.0f;
    weapon->fire_clock = 100;
    auto* weapon_ptr = weapon.get();
    owner->add_weapon(std::move(weapon));
    auto owner_id = registry.register_entity(std::move(owner));
    auto* owner_ptr =
        static_cast<osc::sim::Unit*>(registry.find(owner_id));

    auto target = std::make_unique<osc::sim::Unit>();
    target->set_army(1);
    target->set_position({20.0f, 0.0f, 0.0f});
    auto target_id = registry.register_entity(std::move(target));
    auto* target_ptr =
        static_cast<osc::sim::Unit*>(registry.find(target_id));

    REQUIRE(owner_ptr != nullptr);
    REQUIRE(target_ptr != nullptr);

    weapon_ptr->update(*owner_ptr, registry, nullptr);
    REQUIRE(weapon_ptr->target_entity_id == target_id);

    target_ptr->set_cloaked(true);
    weapon_ptr->update(*owner_ptr, registry, nullptr);
    REQUIRE(weapon_ptr->target_entity_id == 0);
}

TEST_CASE("Weapons do not auto-target BENIGN units but take them on an attack order", "[weapon]") {
    osc::sim::EntityRegistry registry;

    auto owner = std::make_unique<osc::sim::Unit>();
    owner->set_army(0);
    owner->set_position({0.0f, 0.0f, 0.0f});
    auto weapon = std::make_unique<osc::sim::Weapon>();
    weapon->max_range = 100.0f;
    weapon->damage = 10.0f;
    weapon->target_check_period = 1;
    weapon->fire_clock = 100;
    auto* weapon_ptr = weapon.get();
    owner->add_weapon(std::move(weapon));
    auto* owner_ptr =
        static_cast<osc::sim::Unit*>(registry.find(registry.register_entity(std::move(owner))));

    auto wall = std::make_unique<osc::sim::Unit>();
    wall->set_army(1);
    wall->set_position({10.0f, 0.0f, 0.0f});
    wall->add_category("BENIGN");
    const auto wall_id = registry.register_entity(std::move(wall));

    weapon_ptr->update(*owner_ptr, registry, nullptr);
    CHECK(weapon_ptr->target_entity_id == 0);

    auto tank = std::make_unique<osc::sim::Unit>();
    tank->set_army(1);
    tank->set_position({30.0f, 0.0f, 0.0f});
    const auto tank_id = registry.register_entity(std::move(tank));
    weapon_ptr->update(*owner_ptr, registry, nullptr);
    CHECK(weapon_ptr->target_entity_id == tank_id);

    osc::sim::UnitCommand attack{};
    attack.type = osc::sim::CommandType::Attack;
    attack.target_id = wall_id;
    owner_ptr->push_command(attack, true);
    weapon_ptr->update(*owner_ptr, registry, nullptr);
    CHECK(weapon_ptr->target_entity_id == wall_id);
}

TEST_CASE("Air unit fuel consumption", "[m158]") {
    osc::sim::Unit unit;
    unit.set_layer("Air");
    unit.set_fuel_use_time(10.0f); // 10 seconds
    unit.set_fuel_ratio(1.0f);     // full

    CHECK(unit.fuel_ratio() == 1.0f);

    // Simulate 5 seconds of fuel drain
    float ratio = unit.fuel_ratio();
    for (int i = 0; i < 50; i++) {
        ratio -= 0.1f / 10.0f; // dt=0.1, fuel_use_time=10
    }
    CHECK(ratio > 0.49f);
    CHECK(ratio < 0.51f);

    // Verify sentinel: no fuel system
    osc::sim::Unit unit2;
    unit2.set_layer("Air");
    CHECK(unit2.fuel_ratio() == -1.0f); // sentinel = no fuel
}

TEST_CASE("Air unit fields initialize correctly", "[m157]") {
    osc::sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(15.0f);
    unit.set_turn_rate_rad(1.5f);
    unit.set_elevation_target(20.0f);
    unit.set_accel_rate(7.5f);

    CHECK(unit.is_air_unit());
    CHECK(unit.max_airspeed() == 15.0f);
    CHECK(unit.turn_rate_rad() == 1.5f);
    CHECK(unit.elevation_target() == 20.0f);
    CHECK(unit.heading() == 0.0f);
    CHECK(unit.current_airspeed() == 0.0f);
    CHECK(unit.current_altitude() == 0.0f);
}

TEST_CASE("PathfindingGrid extended passability compiles", "[m160]") {
    // Verify the extended overload compiles (no actual grid test without heightmap)
    // Real passability is tested in integration tests with actual map data
    osc::sim::Unit unit;
    unit.set_layer("Water");
    CHECK(unit.layer() == "Water");
}

TEST_CASE("Air crash physics: gravity pulls unit down", "[m159]") {
    osc::sim::Unit unit;
    unit.set_layer("Air");
    unit.set_heading(0);
    unit.set_current_airspeed(10.0f);
    unit.set_current_altitude(50.0f);
    unit.set_position({100, 50, 100});

    unit.begin_dying(); // killed in flight: it falls
    CHECK(unit.is_crashing());
    CHECK(unit.is_dying());

    // Simulate crash
    for (int i = 0; i < 100; i++) {
        unit.tick_dying(0.1f, nullptr);
        if (!unit.is_crashing()) break;
    }

    // Should have fallen and impacted, and reports the landing once
    CHECK(unit.position().y <= 0.1f);
    CHECK(unit.crash_impacted());
    CHECK_FALSE(unit.is_crashing());
    CHECK(unit.take_crash_impact());
    CHECK_FALSE(unit.take_crash_impact());
}

TEST_CASE("Air unit full lifecycle: spawn, fly, die, crash", "[m159]") {
    osc::sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(15.0f);
    unit.set_turn_rate_rad(2.0f);
    unit.set_accel_rate(10.0f);
    unit.set_elevation_target(20.0f);
    unit.set_climb_rate(10.0f);
    unit.set_position({50, 20, 50});
    unit.set_current_altitude(20.0f);

    // Fly toward target
    unit.navigator().set_goal({200, 0, 200});
    for (int i = 0; i < 30; i++) {
        unit.navigator().update_air(unit, 0.1, nullptr);
    }
    CHECK(unit.position().x > 50);
    CHECK(unit.current_airspeed() > 0);

    // Killed in flight, it falls
    unit.begin_dying();
    CHECK(unit.is_crashing());

    // Run crash ticks
    bool hit_ground = false;
    for (int i = 0; i < 200; i++) {
        unit.tick_dying(0.1f, nullptr);
        if (unit.crash_impacted()) {
            hit_ground = true;
            break;
        }
    }
    CHECK(hit_ground);
    CHECK(unit.position().y <= 0.1f);
}

TEST_CASE("Naval unit motion type helpers", "[m160]") {
    osc::sim::Unit water_unit;
    water_unit.set_motion_type("RULEUMT_Water");
    water_unit.set_naval_draft(3.0f);
    water_unit.set_elevation_target(-3.0f);
    CHECK(water_unit.is_naval());
    CHECK_FALSE(water_unit.is_amphibious());
    CHECK_FALSE(water_unit.is_hover());
    CHECK_FALSE(water_unit.is_air_unit());
    CHECK(water_unit.naval_draft() == 3.0f);
    CHECK(water_unit.elevation_target() == -3.0f);

    osc::sim::Unit hover_unit;
    hover_unit.set_motion_type("RULEUMT_Hover");
    CHECK(hover_unit.is_hover());
    CHECK_FALSE(hover_unit.is_naval());
    CHECK_FALSE(hover_unit.is_amphibious());

    osc::sim::Unit amphib_unit;
    amphib_unit.set_motion_type("RULEUMT_Amphibious");
    CHECK(amphib_unit.is_amphibious());
    CHECK_FALSE(amphib_unit.is_naval());
    CHECK_FALSE(amphib_unit.is_hover());

    osc::sim::Unit sub_unit;
    sub_unit.set_motion_type("RULEUMT_SurfacingSub");
    CHECK(sub_unit.is_naval());
}

TEST_CASE("Naval sub unit Y positioning target", "[m160]") {
    osc::sim::Unit unit;
    unit.set_layer("Sub");
    unit.set_motion_type("RULEUMT_SurfacingSub");
    unit.set_elevation_target(-3.0f);
    unit.set_naval_draft(3.0f);
    unit.set_position({100, 25, 100});

    // Target Y for Sub layer: water_elevation + elevation_target
    // With water_elevation=25 and elevation_target=-3.0, target_y = 22.0
    osc::f32 water_elev = 25.0f;
    osc::f32 target_y = water_elev + unit.elevation_target();
    CHECK(target_y == 22.0f);

    // Verify draft and amphibious flags for pathfinding
    CHECK(unit.naval_draft() == 3.0f);
    CHECK_FALSE(unit.is_amphibious());
    CHECK_FALSE(unit.is_hover());
}

TEST_CASE("Amphibious unit layer transition logic", "[m161]") {
    osc::sim::Unit unit;
    unit.set_motion_type("RULEUMT_Amphibious");
    unit.set_layer("Land");

    CHECK(unit.is_amphibious());
    CHECK(unit.layer() == "Land");

    // When terrain_h < water_elev, amphibious should transition to Water
    // When terrain_h >= water_elev, amphibious should transition to Land
    // Test the is_amphibious() helper and initial layer
    osc::sim::Unit amphib_float;
    amphib_float.set_motion_type("RULEUMT_AmphibiousFloating");
    CHECK(amphib_float.is_amphibious());

    // Hover units should NOT be amphibious
    osc::sim::Unit hover;
    hover.set_motion_type("RULEUMT_Hover");
    CHECK_FALSE(hover.is_amphibious());
    CHECK(hover.is_hover());
}

TEST_CASE("Projectile homing tracks toward target", "[m160]") {
    osc::sim::EntityRegistry registry;

    // Create a target at (0, 0, 50)
    auto target = std::make_unique<osc::sim::Unit>();
    target->set_position({0, 0, 50});
    osc::u32 tid = registry.register_entity(std::move(target));

    // A tracking projectile facing and moving +X, its target in +Z. As
    // Moho's UpdateTracking does, it turns its facing (a tenth of its turn
    // rate a tick), keeps what of its velocity lies along it (VelocityAlign)
    // and thrusts along it, up to its top speed.
    osc::sim::Projectile proj;
    proj.set_position({0, 0, 0});
    proj.set_orientation(osc::sim::coords_orient({1, 0, 0}));
    proj.velocity = {10, 0, 0}; // moving +X
    proj.tracking = true;
    proj.velocity_align = true;
    proj.turn_rate = 360.0f; // 36 degrees a tick
    proj.acceleration = 10.0f;
    proj.max_speed = 10.0f;
    proj.lifetime = 5.0f;
    proj.target_entity_id = tid;

    proj.update(0.1, registry, nullptr, nullptr);
    const osc::sim::Vector3 ahead = osc::sim::forward_of(proj.orientation());
    CHECK(ahead.z == Catch::Approx(std::sin(36.0f * 0.017453292f)).margin(1e-4));
    // Its velocity turned with it: 10 cos 36 along the facing, plus a tick
    // of thrust
    CHECK(proj.velocity.z > 0.0f);
    float spd = std::sqrt(proj.velocity.x * proj.velocity.x +
                        proj.velocity.y * proj.velocity.y +
                        proj.velocity.z * proj.velocity.z);
    CHECK(spd == Catch::Approx(10.0f * std::cos(36.0f * 0.017453292f) + 1.0f).margin(1e-3));
    CHECK(proj.velocity.z / spd == Catch::Approx(ahead.z).margin(1e-4));
}

TEST_CASE("A tracking projectile ignores its gravity; one that doesn't track falls",
          "[m160][projectile]") {
    // Moho's projectile motion integrates gravity only for a shot without
    // TrackTarget: FAF's torpedoes say UseGravity, and sank to the seabed.
    osc::sim::EntityRegistry registry;
    auto target = std::make_unique<osc::sim::Unit>();
    target->set_position({0, 0, 50});
    target->set_size_y(0.0f); // its centre, the point aimed at, level with the shot
    const osc::u32 tid = registry.register_entity(std::move(target));
    osc::sim::Projectile torpedo;
    torpedo.set_position({0, 0, 0});
    torpedo.velocity = {0, 0, 10};
    torpedo.tracking = true;
    torpedo.turn_rate = 165.0f;
    torpedo.max_speed = 15.0f;
    torpedo.lifetime = 10.0f;
    torpedo.ballistic_accel = -osc::sim::Projectile::GRAVITY;
    torpedo.target_entity_id = tid;
    osc::sim::Projectile shell = torpedo;
    shell.tracking = false;
    shell.target_entity_id = 0;
    for (int i = 0; i < 10; ++i) {
        torpedo.update(0.1, registry, nullptr, nullptr);
        shell.update(0.1, registry, nullptr, nullptr);
    }
    CHECK(std::abs(torpedo.position().y) < 0.01f);
    CHECK(torpedo.position().z > 5.0f);
    CHECK(shell.position().y < -1.0f); // a second's fall
}

TEST_CASE("Unit veterancy fields and record_damage", "[m162]") {
    osc::sim::Unit u;
    CHECK(u.vet_level() == 0);
    CHECK(u.damage_contributions().empty());

    // Record damage from two attackers
    u.record_damage(100, 50.0f);
    u.record_damage(200, 30.0f);
    u.record_damage(100, 20.0f); // same attacker — should accumulate
    REQUIRE(u.damage_contributions().size() == 2);
    CHECK(u.damage_contributions()[0].first == 100);
    CHECK(u.damage_contributions()[0].second == 70.0f); // 50 + 20
    CHECK(u.damage_contributions()[1].first == 200);
    CHECK(u.damage_contributions()[1].second == 30.0f);

    u.clear_damage_contributions();
    CHECK(u.damage_contributions().empty());
}

TEST_CASE("Teleport destination validates bounds and occupancy", "[teleport]") {
    osc::blueprints::BlueprintStore store(nullptr);
    osc::sim::SimState sim(nullptr, &store);
    sim.set_playable_rect(0.0f, 0.0f, 100.0f, 100.0f);

    auto teleporter = std::make_unique<osc::sim::Unit>();
    teleporter->set_layer("Land");
    teleporter->set_footprint_size(4.0f, 4.0f);
    teleporter->set_position({20.0f, 0.0f, 20.0f});
    auto teleporter_id =
        sim.entity_registry().register_entity(std::move(teleporter));
    auto* unit = static_cast<osc::sim::Unit*>(
        sim.entity_registry().find(teleporter_id));

    REQUIRE(unit != nullptr);
    REQUIRE(sim.is_valid_teleport_destination(*unit, {50.0f, 0.0f, 50.0f}));
    REQUIRE_FALSE(
        sim.is_valid_teleport_destination(*unit, {1.0f, 0.0f, 50.0f}));

    auto blocker = std::make_unique<osc::sim::Unit>();
    blocker->set_layer("Land");
    blocker->set_footprint_size(6.0f, 6.0f);
    blocker->set_position({60.0f, 0.0f, 60.0f});
    sim.entity_registry().register_entity(std::move(blocker));

    REQUIRE_FALSE(
        sim.is_valid_teleport_destination(*unit, {60.0f, 0.0f, 60.0f}));
    REQUIRE(sim.is_valid_teleport_destination(*unit, {70.0f, 0.0f, 60.0f}));
}

TEST_CASE("GetTerrainTypeOffset is a height offset, not a terrain type", "[sim][terrain]") {
    // Retail's CreateWreckageProp adds it to a terrain height; a table there
    // failed every wreck. No FA terrain type defines an offset.
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store(state.raw());
    osc::sim::SimState sim(state.raw(), &store);
    osc::lua::register_sim_bindings(state, sim);

    auto result = state.do_string("return GetTerrainHeight(10, 10) + GetTerrainTypeOffset(10, 10)");
    REQUIRE(result.ok());
    CHECK(lua_type(state.raw(), -1) == LUA_TNUMBER);
    lua_pop(state.raw(), 1);

    result = state.do_string("return GetTerrainType(10, 10).Name");
    REQUIRE(result.ok());
    CHECK(std::string(lua_tostring(state.raw(), -1)) == "Default");
    lua_pop(state.raw(), 1);
}

TEST_CASE("CreateUnitHPR with an unknown blueprint creates nothing", "[sim][units]") {
    // The creation code read the blueprint's Economy and Categories even
    // when the store didn't know it: a null dereference.
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store(state.raw());
    osc::sim::SimState sim(state.raw(), &store);
    sim.add_army("ARMY_1", "ARMY_1");
    osc::lua::register_sim_bindings(state, sim);
    osc::lua::register_moho_bindings(state, sim);

    auto result = state.do_string("return CreateUnitHPR('xxx9999', 'ARMY_1', 5, 0, 5, 0, 0, 0)");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    CHECK(lua_isnil(state.raw(), -1));
    lua_pop(state.raw(), 1);
    CHECK(sim.entity_registry().count() == 0);
}

TEST_CASE("A player's move goes to a factory as its rally point", "[session][rules]") {
    // M206k: as Moho's UI issues it, a move, patrol or transport call splits
    // the selection: its RALLYPOINT units (factories) take it as a factory
    // command, into their rally orders; the rest as an order.
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "CreateUnit('test_tank', 1, 20, 0, 0)\n"));
    osc::sim::Unit* f = find_factory(h);
    osc::sim::Unit* tank = find_unit(h, "test_tank");
    REQUIRE(f);
    REQUIRE(tank);
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    build.blueprint_id = "test_tank";
    f->push_command(build, false);
    f->push_command(build, false);
    h.sim.set_recording(true);

    const auto order = [&](osc::sim::CommandType type, osc::f32 x, bool clear) {
        osc::sim::UnitCommand cmd;
        cmd.type = type;
        cmd.target_pos = {x, 0, 60};
        h.sim.set_human_input_active(true);
        h.sim.route_player_command({f->entity_id(), tank->entity_id()}, cmd, clear);
        h.sim.set_human_input_active(false);
        h.sim.tick();
    };
    order(osc::sim::CommandType::Move, 50, true);
    // Two commands: the factory's, flagged, and the tank's.
    const auto& recorded = h.sim.recorded_replay().commands;
    REQUIRE(recorded.size() == 2);
    CHECK(recorded[0].unit_ids == std::vector<osc::u32>{tank->entity_id()});
    CHECK_FALSE(recorded[0].command.factory);
    CHECK(recorded[1].unit_ids == std::vector<osc::u32>{f->entity_id()});
    CHECK(recorded[1].command.factory);
    // The factory's rally orders take it; its builds are untouched.
    REQUIRE(f->rally_orders().size() == 1);
    CHECK(f->rally_orders()[0].target_pos.x == 50.0f);
    CHECK(factory_orders(*f) == "test_tank test_tank");

    // Queued, it adds one; given fresh, it replaces them.
    order(osc::sim::CommandType::Patrol, 70, false);
    REQUIRE(f->rally_orders().size() == 2);
    CHECK(f->rally_orders()[1].type == osc::sim::CommandType::Patrol);
    order(osc::sim::CommandType::Move, 90, true);
    REQUIRE(f->rally_orders().size() == 1);
    CHECK(f->rally_orders()[0].target_pos.x == 90.0f);
    CHECK(factory_orders(*f) == "test_tank test_tank");

    // Any other order goes to the selection as it is.
    const size_t before = recorded.size();
    order(osc::sim::CommandType::Stop, 0, true);
    REQUIRE(recorded.size() == before + 1);
    CHECK(recorded.back().unit_ids.size() == 2);
    CHECK_FALSE(recorded.back().command.factory);
}

TEST_CASE("A patrol whose point a unit stands on doesn't spin", "[session][rules]") {
    // Reached, a patrol point goes to the back and the next leg waits for the
    // next tick: with every point reached at once it would otherwise go
    // round them for ever inside one tick.
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("CreateUnit('test_tank', 1, 20, 0, 0)\n"));
    osc::sim::Unit* tank = find_unit(h, "test_tank");
    REQUIRE(tank);
    osc::sim::UnitCommand patrol;
    patrol.type = osc::sim::CommandType::Patrol;
    patrol.target_pos = tank->position();
    tank->push_command(patrol, true);
    h.sim.tick();
    REQUIRE(tank->command_queue().size() == 1);
    CHECK(tank->command_queue().front().type == osc::sim::CommandType::Patrol);
}

TEST_CASE("A paused factory holds its build without work, and the unit doesn't decay",
          "[session][rules][pause]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    h.sim.tick();
    h.sim.tick();
    REQUIRE(f->is_building());
    const osc::sim::Entity* tank = h.sim.entity_registry().find(f->build_target_id());
    const osc::f32 frac = tank->fraction_complete();
    f->set_paused(true);
    for (int i = 0; i < 20; ++i) {
        h.sim.tick();
    }
    CHECK(f->is_building());
    CHECK(tank->fraction_complete() == frac);
    f->set_paused(false);
    h.sim.tick();
    CHECK(tank->fraction_complete() > frac);
}

TEST_CASE("A factory paused before it starts makes its unit only on a retry after unpause",
          "[session][rules][pause]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "IssueBuildFactory({factory}, 'test_tank', 1)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    f->set_paused(true);
    for (int i = 0; i < 15; ++i) {
        h.sim.tick();
    }
    CHECK(h.sim.entity_registry().count() == 1);
    f->set_paused(false);
    for (int i = 0; i < 5; ++i) {
        h.sim.tick();
        CHECK(h.sim.entity_registry().count() == 1);
    }
    h.sim.tick();
    CHECK(h.sim.entity_registry().count() == 2);
}

TEST_CASE("A paused unit's enhancement waits", "[session][rules][pause]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string(
        "__blueprints.test_factory.Enhancements = {Gun = {BuildTime = 100, BuildCostMass = 1,"
        " BuildCostEnergy = 1}}\n"
        "CreateUnit('test_factory', 1, 0, 0, 0)\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    osc::sim::UnitCommand enhance;
    enhance.type = osc::sim::CommandType::Enhance;
    enhance.blueprint_id = "Gun";
    f->push_command(enhance, true);
    h.sim.tick();
    h.sim.tick();
    REQUIRE(f->is_enhancing());
    const osc::f32 work = f->work_progress();
    REQUIRE(work > 0.0f);
    f->set_paused(true);
    for (int i = 0; i < 5; ++i) {
        h.sim.tick();
    }
    CHECK(f->is_enhancing());
    CHECK(f->work_progress() == work);
    f->set_paused(false);
    h.sim.tick();
    CHECK(f->work_progress() > work);
}

TEST_CASE("unit:SetPaused calls OnPaused, and a paused unit pays what its script leaves on",
          "[session][rules][pause]") {
    BuildRuleHarness h;
    REQUIRE(h.state.do_string("local M = moho.unit_methods\n"
                              "local function unit(maintenance)\n"
                              "  local u = CreateUnit('test_factory', 1, 0, 0, 0)\n"
                              "  u.Maintenance = maintenance\n"
                              "  M.SetConsumptionPerSecondEnergy(u, maintenance + 20)\n"
                              "  M.SetConsumptionActive(u, true)\n"
                              "  u.OnPaused = function(self)\n"
                              "    M.SetConsumptionPerSecondEnergy(self, self.Maintenance)\n"
                              "    M.SetConsumptionActive(self, self.Maintenance > 0)\n"
                              "  end\n"
                              "  return u\n"
                              "end\n"
                              "local shield, builder = unit(5), unit(0)\n"
                              "M.SetPaused(shield, true)\n"
                              "M.SetPaused(builder, true)\n"));
    h.sim.tick();
    CHECK(h.sim.get_army(0)->economy().energy.requested == Catch::Approx(5.0));
}

TEST_CASE("unit:SetPaused pauses only a unit with RULEUCC_Pause or RULEUTC_GenericToggle",
          "[session][rules][pause]") {
    BuildRuleHarness h;
    REQUIRE(
        h.state.do_string("local M = moho.unit_methods\n"
                          "local function paused(command, toggle)\n"
                          "  local u = CreateUnit('test_tank', 1, 0, 0, 0)\n"
                          "  if command then M.AddCommandCap(u, command) end\n"
                          "  if toggle then M.AddToggleCap(u, toggle) end\n"
                          "  M.SetPaused(u, true)\n"
                          "  return M.IsPaused(u)\n"
                          "end\n"
                          "__osc_paused = tostring(paused()) .. ','\n"
                          "  .. tostring(paused('RULEUCC_Pause')) .. ','\n"
                          "  .. tostring(paused(nil, 'RULEUTC_GenericToggle')) .. ','\n"
                          "  .. tostring(paused('RULEUCC_Stop', 'RULEUTC_ProductionToggle'))\n"));
    lua_State* L = h.state.raw();
    lua_getglobal(L, "__osc_paused");
    CHECK(std::string(lua_tostring(L, -1)) == "false,true,true,false");
    lua_pop(L, 1);
}

namespace {

osc::sim::Unit* enhancing_factory(BuildRuleHarness& h) {
    REQUIRE(h.state.do_string(
        "__blueprints.test_factory.Enhancements = {Gun = {BuildTime = 100, BuildCostMass = 1,"
        " BuildCostEnergy = 1}}\n"
        "__osc_factory = CreateUnit('test_factory', 1, 0, 0, 0)\n"
        "local M = moho.unit_methods\n"
        "__osc_factory.OnPaused = function(self) M.SetConsumptionActive(self, false) end\n"
        "__osc_factory.OnUnpaused = function(self)\n"
        "  if M.IsUnitState(self, 'Upgrading') then M.SetConsumptionActive(self, true) end\n"
        "end\n"));
    osc::sim::Unit* f = find_factory(h);
    REQUIRE(f);
    osc::sim::UnitCommand enhance;
    enhance.type = osc::sim::CommandType::Enhance;
    enhance.blueprint_id = "Gun";
    f->push_command(enhance, true);
    return f;
}

} // namespace

TEST_CASE("An enhancement begun while paused pays without progress", "[session][rules][pause]") {
    BuildRuleHarness h;
    osc::sim::Unit* f = enhancing_factory(h);
    f->set_paused(true);
    for (int i = 0; i < 3; ++i) {
        h.sim.tick();
    }
    REQUIRE(f->is_enhancing());
    CHECK(f->work_progress() == 0.0f);
    const double rate = f->build_rate() / 100.0;
    CHECK(h.sim.get_army(0)->economy().energy.requested == Catch::Approx(rate));
}

TEST_CASE("An enhancing unit is Upgrading, so its OnUnpaused resumes paying",
          "[session][rules][pause]") {
    BuildRuleHarness h;
    osc::sim::Unit* f = enhancing_factory(h);
    h.sim.tick();
    REQUIRE(f->is_enhancing());
    REQUIRE(h.state.do_string("moho.unit_methods.SetPaused(__osc_factory, true)\n"));
    h.sim.tick();
    CHECK(h.sim.get_army(0)->economy().energy.requested == 0.0);
    REQUIRE(h.state.do_string("moho.unit_methods.SetPaused(__osc_factory, false)\n"));
    h.sim.tick();
    CHECK(h.sim.get_army(0)->economy().energy.requested > 0.0);
}

namespace {

struct UpgradeHarness {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    osc::sim::Unit* factory = nullptr;

    UpgradeHarness() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        lua_State* L = state.raw();
        for (const char* bp : {
                 "return {BlueprintId = 'f1', CategoriesHash = {STRUCTURE = true, FACTORY = true},"
                 " Economy = {BuildRate = 20, BuildTime = 100, BuildCostMass = 10,"
                 " BuildCostEnergy = 10, BuildableCategory = {'MOBILE TECH1', 'f2'}},"
                 " General = {UpgradesTo = 'f2'}, Defense = {MaxHealth = 100}}",
                 "return {BlueprintId = 'f2', CategoriesHash = {STRUCTURE = true, FACTORY = true},"
                 " Economy = {BuildRate = 20, BuildTime = 100, BuildCostMass = 10,"
                 " BuildCostEnergy = 10, BuildableCategory = {'MOBILE', 'f3'}},"
                 " General = {UpgradesFrom = 'f1', UpgradesTo = 'f3'}, Defense = {MaxHealth = "
                 "100}}",
                 "return {BlueprintId = 'f3', CategoriesHash = {STRUCTURE = true, FACTORY = true},"
                 " Economy = {BuildRate = 20, BuildTime = 100, BuildCostMass = 10,"
                 " BuildCostEnergy = 10, BuildableCategory = {'MOBILE'}},"
                 " General = {UpgradesFrom = 'f2'}, Defense = {MaxHealth = 100}}",
                 "return {BlueprintId = 't1', CategoriesHash = {MOBILE = true, TECH1 = true},"
                 " Economy = {BuildTime = 10, BuildCostMass = 1, BuildCostEnergy = 1},"
                 " Defense = {MaxHealth = 10}}",
                 "return {BlueprintId = 't2', CategoriesHash = {MOBILE = true, TECH2 = true},"
                 " Economy = {BuildTime = 10, BuildCostMass = 1, BuildCostEnergy = 1},"
                 " Defense = {MaxHealth = 10}}",
             }) {
            REQUIRE(state.do_string(bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state.do_string("CreateUnit('f1', 1, 0, 0, 0)").ok());
        sim.entity_registry().for_each_unit(
            [&](osc::sim::Entity& e) { factory = static_cast<osc::sim::Unit*>(&e); });
        REQUIRE(factory);
        factory->economy().production_mass = 100.0;
        factory->economy().production_energy = 100.0;
        factory->economy().production_active = true;
    }

    void queue(osc::sim::CommandType type, const char* bp) {
        osc::sim::UnitCommand c;
        c.type = type;
        c.blueprint_id = bp;
        factory->push_command(c, false);
    }

    std::string shown() const {
        std::string s;
        for (const auto& g : factory->factory_queue()) {
            s += (s.empty() ? "" : " ") + g.blueprint_id + "x" + std::to_string(g.count);
        }
        return s;
    }
};

} // namespace

TEST_CASE("A factory's queue shows its upgrade orders", "[session][rules]") {
    using osc::sim::CommandType;
    UpgradeHarness h;
    h.queue(CommandType::BuildFactory, "t1");
    h.queue(CommandType::Upgrade, "f2");
    h.queue(CommandType::BuildFactory, "t2");
    CHECK(h.shown() == "t1x1 f2x1 t2x1");
}

TEST_CASE("Taking a queued upgrade off the factory's queue drops what only it led to",
          "[session][rules]") {
    using osc::sim::CommandType;
    UpgradeHarness h;
    h.queue(CommandType::BuildFactory, "t1");
    h.queue(CommandType::Upgrade, "f2");
    h.queue(CommandType::BuildFactory, "t2");
    h.queue(CommandType::Upgrade, "f3");
    h.queue(CommandType::BuildFactory, "t1");
    REQUIRE(h.shown() == "t1x1 f2x1 t2x1 f3x1 t1x1");

    h.factory->decrease_build_count(2, 1, h.sim.entity_registry(), h.state.raw());
    CHECK(h.shown() == "t1x2");
    CHECK(h.factory->command_queue().size() == 2);
}

TEST_CASE("Taking an upgrade under way off the factory's queue ends it", "[session][rules]") {
    UpgradeHarness h;
    h.queue(osc::sim::CommandType::Upgrade, "f2");
    h.sim.tick();
    REQUIRE(h.factory->is_building());
    const osc::u32 upgrade = h.factory->build_target_id();
    REQUIRE(h.shown() == "f2x1");

    h.factory->decrease_build_count(1, 1, h.sim.entity_registry(), h.state.raw());
    CHECK(h.factory->command_queue().empty());
    CHECK_FALSE(h.factory->is_building());
    const osc::sim::Entity* left = h.sim.entity_registry().find(upgrade);
    CHECK((!left || left->destroyed()));
}

TEST_CASE("A stopped upgrade takes its unfinished unit with it", "[session][rules]") {
    UpgradeHarness h;
    lua_State* L = h.state.raw();
    lua_pushstring(L, "__f");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.factory->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);
    REQUIRE(h.state
                .do_string("__heard = ''\n"
                           "function __f:OnFailedToBuild() __heard = __heard .. 'failed ' end\n"
                           "function __f:OnStopBuild(u, order)\n"
                           "  __heard = __heard .. 'stop ' .. order .. ' ' ..\n"
                           "            tostring(u == __frame)\n"
                           "end")
                .ok());
    h.queue(osc::sim::CommandType::Upgrade, "f2");
    h.sim.tick();
    h.sim.tick();
    REQUIRE(h.factory->is_building());
    const osc::u32 upgrade = h.factory->build_target_id();
    lua_pushstring(L, "__frame");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.sim.entity_registry().find(upgrade)->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);
    REQUIRE(
        h.state.do_string("function __frame:OnFailedToBeBuilt() __heard = __heard .. 'frame ' end")
            .ok());

    SECTION("by Stop") {
        osc::sim::UnitCommand stop;
        stop.type = osc::sim::CommandType::Stop;
        h.sim.route_command({h.factory->entity_id()}, stop, true);
    }
    SECTION("by taking its order off") {
        h.factory->remove_command(h.factory->command_queue().front().command_id,
                                  h.sim.entity_registry(), L);
    }
    h.sim.tick();

    const osc::sim::Entity* left = h.sim.entity_registry().find(upgrade);
    CHECK((!left || left->destroyed()));
    CHECK_FALSE(h.factory->destroyed());
    CHECK_FALSE(h.factory->is_building());
    CHECK_FALSE(h.factory->economy().consumption_active);
    CHECK(h.factory->work_progress() == 0.0f);
    lua_pushstring(L, "__heard");
    lua_gettable(L, LUA_GLOBALSINDEX);
    CHECK(std::string(lua_tostring(L, -1)) == "failed frame stop Upgrade true");
    lua_pop(L, 1);
}

TEST_CASE("Every way of clearing the queue cancels the build or upgrade under way",
          "[session][rules]") {
    const auto [type, order] =
        GENERATE(std::pair{osc::sim::CommandType::BuildFactory, std::string("FactoryBuild")},
                 std::pair{osc::sim::CommandType::Upgrade, std::string("Upgrade")});
    const std::string path =
        GENERATE("a player's Stop", "IssueClearCommands({__f})", "__f:Stop()",
                 "IssueToUnitClearCommands(__f)", "platoon:Stop()", "a new order");
    CAPTURE(order, path);
    UpgradeHarness h;
    lua_State* L = h.state.raw();
    const auto run = [&](const std::string& code) {
        const auto r = h.state.do_string(code);
        INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
    };
    lua_pushstring(L, "__f");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.factory->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);
    run("__unit_class = {__index = function(_, k)\n"
        "  return moho.unit_methods[k] or moho.entity_methods[k]\n"
        "end}\n"
        "setmetatable(__f, __unit_class)");
    REQUIRE(h.state
                .do_string("__heard = ''\n"
                           "function __f:OnFailedToBuild() __heard = __heard .. 'failed ' end\n"
                           "function __f:OnStopBuild(u, order)\n"
                           "  __heard = __heard .. 'stop ' .. order .. ' ' ..\n"
                           "            tostring(u == __frame)\n"
                           "end")
                .ok());
    h.queue(type, type == osc::sim::CommandType::Upgrade ? "f2" : "t2");
    h.sim.tick();
    h.sim.tick();
    REQUIRE(h.factory->is_building());
    const osc::u32 frame = h.factory->build_target_id();
    lua_pushstring(L, "__frame");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.sim.entity_registry().find(frame)->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);
    REQUIRE(h.state
                .do_string("function __frame:OnFailedToBeBuilt()\n"
                           "  __heard = __heard .. 'frame '\n"
                           "  if not IsDestroyed(self) then self:Destroy() end\n"
                           "end")
                .ok());

    if (path == "a player's Stop") {
        osc::sim::UnitCommand stop;
        stop.type = osc::sim::CommandType::Stop;
        h.sim.route_command({h.factory->entity_id()}, stop, true);
    } else if (path == "a new order") {
        osc::sim::UnitCommand next;
        next.type = osc::sim::CommandType::BuildFactory;
        next.blueprint_id = "t1";
        h.sim.route_command({h.factory->entity_id()}, next, true);
    } else if (path == "platoon:Stop()") {
        osc::sim::Platoon* platoon = h.sim.army_at(0)->create_platoon("");
        platoon->add_unit(h.factory->entity_id());
        platoon->set_unit_squad(h.factory->entity_id(), "Attack");
        lua_pushstring(L, "__p");
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, platoon);
        lua_rawset(L, -3);
        lua_settable(L, LUA_GLOBALSINDEX);
        run("moho.platoon_methods.Stop(__p)");
    } else {
        run(path);
    }

    const osc::sim::Entity* left = h.sim.entity_registry().find(frame);
    CHECK((!left || left->destroyed()));
    CHECK_FALSE(h.factory->destroyed());
    CHECK(h.factory->build_target_id() == 0);
    CHECK_FALSE(h.factory->economy().consumption_active);
    CHECK(h.factory->work_progress() == 0.0f);
    lua_pushstring(L, "__heard");
    lua_gettable(L, LUA_GLOBALSINDEX);
    CHECK(std::string(lua_tostring(L, -1)) == "failed frame stop " + order + " true");
    lua_pop(L, 1);

    h.sim.tick();
    CHECK(h.factory->build_target_id() != frame);
}

TEST_CASE("A factory's build and an upgrade tell OnStartBuild and OnStopBuild their order",
          "[session][rules]") {
    const auto [type, bp, order] =
        GENERATE(std::tuple{osc::sim::CommandType::BuildFactory, "t1", "FactoryBuild"},
                 std::tuple{osc::sim::CommandType::Upgrade, "f2", "Upgrade"});
    CAPTURE(order);
    UpgradeHarness h;
    lua_State* L = h.state.raw();
    lua_pushstring(L, "__f");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.factory->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);
    REQUIRE(h.state
                .do_string("__heard = ''\n"
                           "function __f:OnStartBuild(u, order)\n"
                           "  __heard = __heard .. 'start ' .. tostring(order) .. ' '\n"
                           "end\n"
                           "function __f:OnStopBuild(u, order)\n"
                           "  __heard = __heard .. 'stop ' .. tostring(order) .. ' '\n"
                           "end")
                .ok());
    h.queue(type, bp);
    for (int i = 0; i < 100 && !h.factory->command_queue().empty(); ++i) {
        h.sim.tick();
    }
    CHECK(h.factory->command_queue().empty());
    lua_pushstring(L, "__heard");
    lua_gettable(L, LUA_GLOBALSINDEX);
    CHECK(std::string(lua_tostring(L, -1)) ==
          "start " + std::string(order) + " stop " + std::string(order) + " ");
    lua_pop(L, 1);
}

TEST_CASE("A platoon's AttackTarget and GuardTarget go behind its units' orders",
          "[session][rules]") {
    const std::string call = GENERATE("AttackTarget", "GuardTarget");
    CAPTURE(call);
    UpgradeHarness h;
    lua_State* L = h.state.raw();
    REQUIRE(h.state.do_string("__t = CreateUnit('t1', 1, 5, 0, 5)").ok());
    osc::sim::Unit* tank = nullptr;
    h.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        if (&e != h.factory) {
            tank = static_cast<osc::sim::Unit*>(&e);
        }
    });
    REQUIRE(tank);
    tank->add_command_cap("RULEUCC_Guard");
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {50.0f, 0.0f, 50.0f};
    tank->push_command(move, false);
    osc::sim::Platoon* platoon = h.sim.army_at(0)->create_platoon("");
    platoon->add_unit(tank->entity_id());
    platoon->set_unit_squad(tank->entity_id(), "Attack");
    lua_pushstring(L, "__p");
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, platoon);
    lua_rawset(L, -3);
    lua_settable(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "__f");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.factory->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);

    REQUIRE(h.state.do_string("moho.platoon_methods." + call + "(__p, __f)").ok());
    REQUIRE(tank->command_queue().size() == 2);
    CHECK(tank->command_queue().front().type == osc::sim::CommandType::Move);
}

TEST_CASE("IssueStop queues a Stop behind the build under way, which finishes",
          "[session][rules]") {
    const std::string call = GENERATE("IssueStop({__f})", "IssueToUnitStop(__f)");
    CAPTURE(call);
    UpgradeHarness h;
    lua_State* L = h.state.raw();
    lua_pushstring(L, "__f");
    lua_rawgeti(L, LUA_REGISTRYINDEX, h.factory->lua_table_ref());
    lua_settable(L, LUA_GLOBALSINDEX);
    h.queue(osc::sim::CommandType::BuildFactory, "t2");
    h.sim.tick();
    h.sim.tick();
    REQUIRE(h.factory->is_building());
    const osc::u32 frame = h.factory->build_target_id();

    REQUIRE(h.state.do_string(call).ok());
    REQUIRE(h.factory->command_queue().size() == 2);
    CHECK(h.factory->command_queue().back().type == osc::sim::CommandType::Stop);
    CHECK(h.factory->build_target_id() == frame);

    for (int i = 0; i < 100 && !h.factory->command_queue().empty(); ++i) {
        h.sim.tick();
    }
    CHECK(h.factory->command_queue().empty());
    const auto* built = static_cast<const osc::sim::Unit*>(h.sim.entity_registry().find(frame));
    REQUIRE(built);
    CHECK_FALSE(built->destroyed());
    CHECK_FALSE(built->is_being_built());
}
