// A projectile blueprint's Physics spreads a .bp leaves out read as Moho's
// RProjectileBlueprintPhysics defaults (faf-re): FAF's cruise missiles add
// MaxSpeedRange to MaxSpeed as they are made.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"
#include "sim/projectile.hpp"

#include <string>

TEST_CASE("A projectile's omitted Physics spreads read as Moho's defaults", "[blueprints]") {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    state.set_blueprint_store(&store);
    osc::lua::register_blueprint_store_bindings(state);
    auto r = state.do_string(R"(
        -- TIFMissileCruise01: a MaxSpeed, no MaxSpeedRange.
        local cruise = {BlueprintId = 'cruise', Physics = {MaxSpeed = 12, TurnRateRange = 4}}
        RegisterProjectileBlueprint(cruise)
        local p = cruise.Physics
        assert(p.MaxSpeedRange == 0, 'MaxSpeedRange ' .. tostring(p.MaxSpeedRange))
        assert(p.MaxSpeed + p.MaxSpeedRange == 12, 'as FAF adds them')
        assert(p.DirectionXRange == 1.5 and p.DirectionZRange == 1.5 and p.DirectionYRange == 0,
               'direction spreads')
        assert(p.TurnRateRange == 4, 'its own spread lost')
        -- Only the spreads: the engine's flight reads the rest as before.
        assert(p.Acceleration == nil and p.Lifetime == nil, 'filled more than the spreads')
        -- One with no Physics at all gets them too.
        local bare = {BlueprintId = 'bare'}
        RegisterProjectileBlueprint(bare)
        assert(bare.Physics and bare.Physics.LifetimeRange == 0, 'no Physics')
    )");
    INFO((r.ok() ? std::string() : r.error().message));
    CHECK(r.ok());
}

TEST_CASE("A projectile takes its blueprint's CollideSurface and CollideEntity", "[blueprints]") {
    // Moho's Projectile sets mCollideSurface and mDoCollision from them as
    // it is made: FAF's UEF build beams end on dummy projectiles made inside
    // the unit being built, which hit nothing.
    osc::lua::LuaState state;
    REQUIRE(state
                .do_string(R"(
        __blueprints = {
            dummy = {Physics = {CollideEntity = false, CollideSurface = false}},
            shell = {Physics = {MaxSpeed = 30}},
        }
    )")
                .ok());
    osc::sim::Projectile dummy;
    dummy.set_blueprint_id("dummy");
    (void)dummy.apply_blueprint_physics(state.raw());
    CHECK_FALSE(dummy.collide_entity);
    CHECK_FALSE(dummy.collide_surface);
    osc::sim::Projectile shell;
    shell.set_blueprint_id("shell");
    (void)shell.apply_blueprint_physics(state.raw());
    CHECK(shell.collide_entity);
    CHECK(shell.collide_surface);
}

TEST_CASE("A projectile leads its target unless its blueprint says not", "[blueprints]") {
    osc::lua::LuaState state;
    REQUIRE(state
                .do_string(R"(
        __blueprints = {
            torpedo = {Physics = {TrackTarget = true, MaxSpeed = 5}},
            dart = {Physics = {TrackTarget = true, MaxSpeed = 30, LeadTarget = false}},
        }
    )")
                .ok());
    osc::sim::Projectile torpedo;
    torpedo.set_blueprint_id("torpedo");
    (void)torpedo.apply_blueprint_physics(state.raw());
    CHECK(torpedo.lead_target);
    osc::sim::Projectile dart;
    dart.set_blueprint_id("dart");
    (void)dart.apply_blueprint_physics(state.raw());
    CHECK_FALSE(dart.lead_target);
}
