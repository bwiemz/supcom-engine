// A projectile blueprint's Physics spreads a .bp leaves out read as Moho's
// RProjectileBlueprintPhysics defaults (faf-re): FAF's cruise missiles add
// MaxSpeedRange to MaxSpeed as they are made.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/entity_registry.hpp"
#include "sim/flight_math.hpp"
#include "sim/projectile.hpp"
#include "sim/projectile_script.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
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

TEST_CASE("A projectile a script makes lives Moho's 15 seconds unless its blueprint says",
          "[blueprints]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    REQUIRE(lua.do_string(R"(
        __blueprints = {
            shell = {Physics = {}},
            timed = {Physics = {Lifetime = 3}},
            typo = {Physics = {LifeTime = 30}},
        }
    )")
                .ok());
    lua_State* L = lua.raw();
    auto parent = std::make_unique<osc::sim::Projectile>();
    parent->set_blueprint_id("shell");
    const osc::u32 id = sim.entity_registry().register_entity(std::move(parent));
    osc::sim::create_projectile_object(
        L, *static_cast<osc::sim::Projectile*>(sim.entity_registry().find(id)), false, true);
    lua_setglobal(L, "parent");
    lua_settop(L, 0);
    const auto lifetime = [&](const std::string& bp) {
        REQUIRE(lua.do_string("child = moho.projectile_methods.CreateChildProjectile(parent, '" +
                              bp + "')")
                    .ok());
        lua_getglobal(L, "child");
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        const auto* child = static_cast<const osc::sim::Projectile*>(lua_touserdata(L, -1));
        lua_settop(L, 0);
        REQUIRE(child);
        return child->lifetime_end;
    };
    CHECK(lifetime("shell") == 150);
    CHECK(lifetime("timed") == 30);
    CHECK(lifetime("typo") == 150);
}

TEST_CASE("A projectile stays upright if its blueprint says StayUpright", "[blueprints]") {
    osc::lua::LuaState state;
    REQUIRE(state
                .do_string(R"(
        __blueprints = {
            upright = {Physics = {StayUpright = true, VelocityAlign = false}},
            tumbling = {Physics = {VelocityAlign = false}},
        }
    )")
                .ok());
    osc::sim::EntityRegistry registry;
    const auto spin = [&](const char* bp) {
        auto made = std::make_unique<osc::sim::Projectile>();
        made->set_blueprint_id(bp);
        (void)made->apply_blueprint_physics(state.raw());
        made->collide_surface = false;
        made->collide_entity = false;
        made->set_lifetime(100.0f, 0);
        made->angular_velocity = {3, 4, 0};
        auto* p = static_cast<osc::sim::Projectile*>(
            registry.find(registry.register_entity(std::move(made))));
        for (int tick = 0; tick < 5; ++tick) {
            p->update(0.1, registry, nullptr);
            registry.advance_coords();
        }
        return p->orientation();
    };
    const osc::sim::Quaternion upright = spin("upright");
    CHECK(osc::sim::quat_rotate(upright, {0, 1, 0}).y == Catch::Approx(1.0f).margin(1e-5));
    const osc::sim::Vector3 ahead = osc::sim::forward_of(upright);
    CHECK(ahead.y == Catch::Approx(0.0f).margin(1e-5));
    CHECK(ahead.z == Catch::Approx(std::cos(2.5f)).margin(1e-4));
    const osc::sim::Quaternion tumbling = spin("tumbling");
    CHECK(osc::sim::quat_rotate(tumbling, {0, 1, 0}).y < 0.5f);
}
