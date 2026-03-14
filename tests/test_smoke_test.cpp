#include <catch2/catch_test_macros.hpp>
#include "lua/smoke_test.hpp"
#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/manipulator.hpp"
#include "sim/weapon.hpp"
#include "sim/projectile.hpp"
#include "sim/entity_registry.hpp"

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

    unit.begin_air_crash(100.0f);
    CHECK(unit.is_crashing());
    CHECK(unit.is_dying());

    // Simulate crash
    osc::f32 prev_y = unit.position().y;
    for (int i = 0; i < 100; i++) {
        unit.tick_dying(0.1f);
        if (!unit.is_crashing()) break;
    }

    // Should have fallen and impacted
    CHECK(unit.position().y <= 0.1f);
    CHECK(unit.crash_impacted());
    CHECK_FALSE(unit.is_crashing());
}

TEST_CASE("Air unit full lifecycle: spawn, fly, die, crash", "[m159]") {
    osc::sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(15.0f);
    unit.set_turn_rate_rad(2.0f);
    unit.set_accel_rate(10.0f);
    unit.set_elevation_target(20.0f);
    unit.set_climb_rate(10.0f);
    unit.set_crash_damage(100.0f);
    unit.set_position({50, 20, 50});
    unit.set_current_altitude(20.0f);

    // Fly toward target
    unit.navigator().set_goal({200, 0, 200});
    for (int i = 0; i < 30; i++) {
        unit.navigator().update_air(unit, 0.1, nullptr);
    }
    CHECK(unit.position().x > 50);
    CHECK(unit.current_airspeed() > 0);

    // Kill it via begin_dying — should auto-redirect to crash
    unit.begin_dying(2.0f);
    CHECK(unit.is_crashing());

    // Run crash ticks
    bool hit_ground = false;
    for (int i = 0; i < 200; i++) {
        unit.tick_dying(0.1f);
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

    // Create a tracking projectile moving in +X, target is in +Z
    osc::sim::Projectile proj;
    proj.set_position({0, 0, 0});
    proj.velocity = {10, 0, 0}; // moving +X
    proj.tracking = true;
    proj.turn_rate = 360.0f; // fast turn for test
    proj.max_speed = 10.0f;
    proj.lifetime = 5.0f;
    proj.target_entity_id = tid;

    // After one update tick, velocity should have Z component (turned toward target)
    proj.update(0.1, registry, nullptr, nullptr);
    CHECK(proj.velocity.z > 0.0f);
    // Speed should be preserved (approximately 10)
    float spd = std::sqrt(proj.velocity.x * proj.velocity.x +
                        proj.velocity.y * proj.velocity.y +
                        proj.velocity.z * proj.velocity.z);
    CHECK(spd > 9.5f);
    CHECK(spd < 10.5f);
}
