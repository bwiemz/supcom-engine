// A projectile blueprint's Physics spreads a .bp leaves out read as Moho's
// RProjectileBlueprintPhysics defaults (faf-re): FAF's cruise missiles add
// MaxSpeedRange to MaxSpeed as they are made.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/projectile.hpp"
#include "sim/projectile_script.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
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

TEST_CASE("A projectile the camera follows tells it, with what made it", "[blueprints]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    REQUIRE(lua.do_string(R"(
        __blueprints = {
            nuke = {Display = {CameraFollowsProjectile = true, CameraFollowTimeout = 5}},
            bomb = {Display = {CameraFollowsProjectile = true}},
            shell = {Display = {}},
        }
    )")
                .ok());
    auto launcher = std::make_unique<osc::sim::Entity>();
    const osc::u32 launcher_id = sim.entity_registry().register_entity(std::move(launcher));
    const auto launch = [&](const char* bp) {
        auto p = std::make_unique<osc::sim::Projectile>();
        p->set_blueprint_id(bp);
        p->launcher_id = launcher_id;
        const osc::u32 id = sim.entity_registry().register_entity(std::move(p));
        osc::sim::create_projectile_object(
            lua.raw(), *static_cast<osc::sim::Projectile*>(sim.entity_registry().find(id)), false,
            true);
        return id;
    };

    const osc::u32 nuke = launch("nuke");
    lua_setglobal(lua.raw(), "nuke");
    lua_settop(lua.raw(), 0);
    launch("shell");
    lua_settop(lua.raw(), 0);
    const osc::u32 bomb = launch("bomb");
    lua_settop(lua.raw(), 0);
    REQUIRE(lua.do_string("moho.projectile_methods.CreateChildProjectile(nuke, 'bomb')").ok());

    const auto& follows = sim.camera_follow_events();
    REQUIRE(follows.size() == 3);
    CHECK(follows[0].source == launcher_id);
    CHECK(follows[0].projectile == nuke);
    CHECK(follows[0].timeout == 5.0f);
    CHECK(follows[1].projectile == bomb);
    CHECK(follows[1].timeout == 1.0f);
    CHECK(follows[2].source == nuke);
}
