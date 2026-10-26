// A projectile a script makes is launched as its blueprint says, as Moho's (faf-re Entity.cpp's
// CreateProjectile and CreateProjectileAtBone, RProjectileBlueprint.cpp): an offset or direction
// the script leaves nil is the blueprint's Position or Direction, spread by its range, and it
// leaves at InitialSpeed spread by InitialSpeedRange. Retail's death sparks and debris fly up.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::Projectile;

namespace {

/// A sim on flat ground at 10 with one army's tank at (20, 20), and projectile blueprints
struct Launch {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    Launch() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(65) * 65, 1280);
        osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.add_army("ARMY_1", "ARMY_1");
        lua_State* L = state.raw();
        const auto add = [&](osc::blueprints::BlueprintType type, const char* bp) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, type, lua_gettop(L));
            lua_pop(L, 1);
        };
        add(osc::blueprints::BlueprintType::Unit,
            "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'}, Defense = {MaxHealth = 100},"
            " SizeX = 1, SizeZ = 1, Physics = {MotionType = 'RULEUMT_Land'}}");
        // As retail's DestructionSpark01, raised as its teleport effects are
        add(osc::blueprints::BlueprintType::Projectile,
            "{BlueprintId = '/spark', Physics = {DirectionX = 0, DirectionXRange = 2,"
            " DirectionY = 1.2, DirectionYRange = 0.6, DirectionZ = 0, DirectionZRange = 2,"
            " InitialSpeed = 5, InitialSpeedRange = 5, PositionY = 3, PositionYRange = 0.5}}");
        // As DebrisBoneAttachHigh01
        add(osc::blueprints::BlueprintType::Projectile,
            "{BlueprintId = '/debris', Physics = {DirectionXRange = 0.4, DirectionY = 1,"
            " DirectionZRange = 0.4, InitialSpeed = 6.5, InitialSpeedRange = 3}}");
        // No direction: Moho's default, up and 1.5 either way across
        add(osc::blueprints::BlueprintType::Projectile,
            "{BlueprintId = '/plain', Physics = {InitialSpeed = 4}}");
        // None at all, and no spread
        add(osc::blueprints::BlueprintType::Projectile,
            "{BlueprintId = '/still', Physics = {DirectionY = 0, DirectionXRange = 0,"
            " DirectionZRange = 0, InitialSpeed = 4}}");
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain EM = moho.entity_methods")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "tank");
        lua_getglobal(L, "Plain");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
        REQUIRE(state.do_string("tank = CreateUnit('tank', 1, 20, 10, 20)").ok());
    }

    /// The projectile `call` (Lua, with `tank`) makes
    Projectile& shot(const std::string& call) {
        const auto made = state.do_string("shot = " + call);
        INFO((made.ok() ? std::string() : made.error().message));
        REQUIRE(made.ok());
        lua_State* L = state.raw();
        lua_getglobal(L, "shot");
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        auto* p = static_cast<Projectile*>(lua_touserdata(L, -1));
        lua_settop(L, 0);
        REQUIRE(p);
        return *p;
    }
};

f32 length(const osc::sim::Vector3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

TEST_CASE("A projectile made with no direction leaves by its blueprint's, spread",
          "[projectile][launch]") {
    Launch w;
    bool left = false;
    bool right = false;
    for (int i = 0; i < 60; ++i) {
        const Projectile& p = w.shot("EM.CreateProjectile(tank, '/spark', 0, 0, 0, nil, nil, nil)");
        // Up (DirectionY 1.2 +- 0.6), out either way (DirectionX +- 2), at 5 +- 5
        REQUIRE(p.velocity.y > 0.0f);
        REQUIRE(length(p.velocity) <= 10.0f + 1e-3f);
        left = left || p.velocity.x < 0.0f;
        right = right || p.velocity.x > 0.0f;
        // The offset given, 0
        CHECK(p.position().y == Approx(10.0f));
    }
    CHECK(left);
    CHECK(right);
}

TEST_CASE("A projectile made with no offset sits at its blueprint's Position, spread",
          "[projectile][launch]") {
    Launch w;
    for (int i = 0; i < 20; ++i) {
        // Retail's quantum warhead effect is made so, with the blueprint alone
        const Projectile& p = w.shot("EM.CreateProjectile(tank, '/spark')");
        CHECK(p.position().x == Approx(20.0f));
        CHECK(p.position().y >= 12.5f - 1e-4f);
        CHECK(p.position().y <= 13.5f + 1e-4f);
    }
}

TEST_CASE("A direction a script gives points the projectile, at its spread InitialSpeed",
          "[projectile][launch]") {
    Launch w;
    const Projectile& p = w.shot("EM.CreateProjectile(tank, '/spark', 0, 0, 0, 2, 0, 0)");
    CHECK(p.velocity.x >= 0.0f);
    CHECK(p.velocity.x <= 10.0f + 1e-3f);
    CHECK(p.velocity.y == Approx(0.0f).margin(1e-5));
    CHECK(p.velocity.z == Approx(0.0f).margin(1e-5));
    // None in its blueprint: Moho's default, up and spread across
    const Projectile& plain = w.shot("EM.CreateProjectile(tank, '/plain', 0, 0, 0, nil, nil, nil)");
    CHECK(plain.velocity.y > 0.0f);
    CHECK(length(plain.velocity) == Approx(4.0f));
    // One that comes to nothing leaves it as made
    const Projectile& still = w.shot("EM.CreateProjectile(tank, '/still', 0, 0, 0, nil, nil, nil)");
    CHECK(length(still.velocity) == Approx(0.0f));
    // And one with no blueprint (Moho's "Invalid blueprint") isn't launched
    const Projectile& none = w.shot("EM.CreateProjectile(tank, '/none', 0, 0, 0, nil, nil, nil)");
    CHECK(length(none.velocity) == Approx(0.0f));
}

TEST_CASE("A projectile made at a bone leaves by its blueprint's direction times its speed",
          "[projectile][launch]") {
    Launch w;
    for (int i = 0; i < 40; ++i) {
        const Projectile& p = w.shot("EM.CreateProjectileAtBone(tank, '/debris', 0)");
        // DirectionY is 1 with no range, so the upward speed is the speed itself: 6.5 +- 3,
        // with the sideways parts on top (the direction isn't made a unit)
        REQUIRE(p.velocity.y >= 3.5f - 1e-4f);
        REQUIRE(p.velocity.y <= 9.5f + 1e-4f);
        CHECK(std::abs(p.velocity.x) <= 0.4f * p.velocity.y + 1e-4f);
        CHECK(std::abs(p.velocity.z) <= 0.4f * p.velocity.y + 1e-4f);
    }
    // Moho's default direction: up at the speed, up to 1.5 times it across
    const Projectile& plain = w.shot("EM.CreateProjectileAtBone(tank, '/plain', 0)");
    CHECK(plain.velocity.y == Approx(4.0f));
    CHECK(std::abs(plain.velocity.x) <= 6.0f + 1e-4f);
    // A direction that comes to nothing: along the bone, level, at the speed
    const Projectile& still = w.shot("EM.CreateProjectileAtBone(tank, '/still', 0)");
    CHECK(length(still.velocity) == Approx(4.0f));
    CHECK(still.velocity.y == Approx(0.0f).margin(1e-5));
}
