#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"
#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

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
    REQUIRE(report.size() >= 1);
    bool found_pcall = false;
    for (auto& e : report) {
        if (e.category == osc::lua::SmokeCategory::PcallError) found_pcall = true;
    }
    REQUIRE(found_pcall);
}

TEST_CASE("ArmyBrain build restriction add/remove/check", "[m154]") {
    osc::sim::ArmyBrain brain;
    REQUIRE_FALSE(brain.is_build_restricted("TECH1 LAND FACTORY"));
    brain.add_build_restriction("TECH1 LAND FACTORY");
    REQUIRE(brain.is_build_restricted("TECH1 LAND FACTORY"));
    brain.remove_build_restriction("TECH1 LAND FACTORY");
    REQUIRE_FALSE(brain.is_build_restricted("TECH1 LAND FACTORY"));
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
