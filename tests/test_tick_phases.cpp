// A tick's phases in Moho's order (faf-re Sim::AdvanceBeat): each army's
// weapons, then every unit's orders, then the Lua threads, then the entities.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Weapon;

namespace {

struct Phases {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    Phases() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(257) * 257, 1280);
        osc::map::Heightmap hm(256, 256, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1}, Physics = {MotionType = 'RULEUMT_Land'}}",
                 "{BlueprintId = 'fac', CategoriesHash = {STRUCTURE = true, FACTORY = true},"
                 " Economy = {BuildRate = 20, BuildTime = 100, BuildCostMass = 10,"
                 " BuildCostEnergy = 10, BuildableCategory = {'MOBILE TECH1'}},"
                 " Defense = {MaxHealth = 100}}",
                 "{BlueprintId = 't1', CategoriesHash = {MOBILE = true, TECH1 = true},"
                 " Economy = {BuildTime = 10, BuildCostMass = 1, BuildCostEnergy = 1},"
                 " Defense = {MaxHealth = 10}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(run("Plain = setmetatable({}, {__index = moho.unit_methods})"
                    " Plain.__index = Plain"));
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"tank", "fac", "t1"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
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

    f32 number(const char* global) {
        lua_State* L = state.raw();
        lua_getglobal(L, global);
        const auto v = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return v;
    }

    std::string text(const char* global) {
        lua_State* L = state.raw();
        lua_getglobal(L, global);
        std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return v;
    }

    Unit& make(const char* global, const char* bp, int army, f32 x, f32 z) {
        REQUIRE(run(std::string(global) + " = CreateUnit('" + bp + "', " + std::to_string(army) +
                    ", " + std::to_string(x) + ", 10, " + std::to_string(z) + ")"));
        lua_State* L = state.raw();
        lua_getglobal(L, global);
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        auto* u = static_cast<Unit*>(lua_touserdata(L, -1));
        lua_settop(L, 0);
        REQUIRE(u);
        return *u;
    }

    Weapon& arm(Unit& u, const char* script) {
        auto gun = std::make_unique<Weapon>();
        gun->max_range = 30.0f;
        gun->damage = 10.0f;
        gun->target_check_period = 1;
        gun->projectile_bp_id = "/shell";
        gun->projectile_physics = Weapon::ProjectilePhysics{};
        if (script) {
            lua_State* L = state.raw();
            lua_getglobal(L, script);
            gun->lua_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            gun->script_class = true;
        }
        u.add_weapon(std::move(gun));
        return *u.weapons().back();
    }
};

} // namespace

TEST_CASE("A unit's weapon takes its attack order's target on the tick after the order runs",
          "[tick-phases]") {
    Phases w;
    Unit& tank = w.make("tank", "tank", 1, 100.0f, 100.0f);
    const Unit& rival = w.make("rival", "tank", 2, 115.0f, 100.0f);
    REQUIRE(w.run(
        "heard = {} gun = {OnGotTarget = function() table.insert(heard, GetGameTick()) end}"));
    Weapon& gun = w.arm(tank, "gun");
    gun.target_check_clock = 1000000;
    w.sim.tick();
    REQUIRE(gun.target_entity_id == 0);

    UnitCommand attack;
    attack.type = CommandType::Attack;
    attack.target_id = rival.entity_id();
    w.sim.route_command({tank.entity_id()}, attack, true);
    const osc::u32 issued = w.sim.tick_count() + 1;
    w.sim.tick();
    REQUIRE_FALSE(tank.command_queue().empty());
    CHECK(gun.target_entity_id == 0);
    CHECK(w.run("assert(table.getn(heard) == 0)"));
    w.sim.tick();
    CHECK(gun.target_entity_id == rival.entity_id());
    CHECK(
        w.run("assert(table.getn(heard) == 1 and heard[1] == " + std::to_string(issued + 1) + ")"));
}

TEST_CASE("A weapon doesn't shoot a target in the tick it picks it", "[tick-phases]") {
    Phases w;
    Unit& tank = w.make("tank", "tank", 1, 100.0f, 100.0f);
    Weapon& gun = w.arm(tank, nullptr);
    w.sim.tick();
    w.sim.tick();
    const Unit& rival = w.make("rival", "tank", 2, 115.0f, 100.0f);
    w.sim.tick();
    REQUIRE(gun.target_entity_id == rival.entity_id());
    CHECK(gun.shots_at_target == 0);
    CHECK(gun.fire_clock == 0);
    w.sim.tick();
    CHECK(gun.shots_at_target == 1);
    CHECK(gun.fire_clock == gun.fire_period());
}

TEST_CASE("A projectile made by the thread OnFire forks is made and moves in the OnFire tick",
          "[tick-phases]") {
    Phases w;
    Unit& shooter = w.make("shooter", "tank", 1, 100.0f, 100.0f);
    w.make("rival", "tank", 2, 115.0f, 100.0f);
    REQUIRE(w.run(R"(
        fired = 0 made = 0 made_x = 0
        gun = {OnFire = function()
            fired = GetGameTick()
            ForkThread(function()
                local shot = moho.entity_methods.CreateProjectile(shooter, '/shell', 0, 1, 0)
                moho.projectile_methods.SetVelocity(shot, 10, 0, 0)
                made = GetGameTick()
                made_x = moho.entity_methods.GetPosition(shot)[1]
            end)
        end})"));
    w.arm(shooter, "gun");
    for (int t = 0; t < 5 && w.number("fired") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("fired") > 0);
    CHECK(w.number("made") == w.number("fired"));
    const osc::sim::Projectile* shot = nullptr;
    w.sim.entity_registry().for_each([&](osc::sim::Entity& e) {
        if (e.is_projectile()) {
            shot = static_cast<const osc::sim::Projectile*>(&e);
        }
    });
    REQUIRE(shot);
    CHECK(shot->position().x > w.number("made_x"));
}

TEST_CASE("Army 0's weapons fire before army 1's, whichever unit was made first", "[tick-phases]") {
    const bool rival_first = GENERATE(false, true);
    CAPTURE(rival_first);
    Phases w;
    REQUIRE(w.run("order = ''"
                  " gun1 = {OnFire = function() order = order .. '1' end}"
                  " gun2 = {OnFire = function() order = order .. '2' end}"));
    Unit* first = nullptr;
    Unit* second = nullptr;
    if (rival_first) {
        second = &w.make("b", "tank", 2, 110.0f, 100.0f);
        first = &w.make("a", "tank", 1, 100.0f, 100.0f);
    } else {
        first = &w.make("a", "tank", 1, 100.0f, 100.0f);
        second = &w.make("b", "tank", 2, 110.0f, 100.0f);
    }
    w.arm(*first, "gun1");
    w.arm(*second, "gun2");
    for (int t = 0; t < 5 && w.text("order").empty(); ++t) {
        w.sim.tick();
    }
    CHECK(w.text("order") == "12");
}

TEST_CASE("An IssueMove from a Lua thread runs at the next tick's orders", "[tick-phases]") {
    Phases w;
    Unit& tank = w.make("tank", "tank", 1, 100.0f, 100.0f);
    tank.add_command_cap("RULEUCC_Move");
    tank.set_max_speed(5.0f);
    tank.set_orientation(osc::sim::euler_to_quat(1.5707964f, 0.0f, 0.0f));
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    tank.set_drive(drive);
    REQUIRE(w.run("ForkThread(function() IssueMove({tank}, {130, 10, 100}) end)"));
    const f32 x = tank.position().x;
    w.sim.tick();
    REQUIRE(tank.command_queue().size() == 1);
    CHECK_FALSE(tank.command_queue().front().begun);
    CHECK(tank.position().x == x);
    w.sim.tick();
    REQUIRE(tank.command_queue().size() == 1);
    CHECK(tank.command_queue().front().begun);
    CHECK(tank.position().x > x);
}

TEST_CASE("A thread OnStartBuild forks runs in the tick of OnStartBuild", "[tick-phases]") {
    Phases w;
    Unit& fac = w.make("fac", "fac", 1, 60.0f, 60.0f);
    fac.economy().production_mass = 100.0;
    fac.economy().production_energy = 100.0;
    fac.economy().production_active = true;
    REQUIRE(w.run("started = 0 ran = 0"
                  " function fac:OnStartBuild(u, order)"
                  "   started = GetGameTick()"
                  "   ForkThread(function() ran = GetGameTick() end)"
                  " end"));
    UnitCommand build;
    build.type = CommandType::BuildFactory;
    build.blueprint_id = "t1";
    fac.push_command(build, false);
    for (int t = 0; t < 10 && w.number("started") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("started") > 0);
    CHECK(w.number("ran") == w.number("started"));
}

TEST_CASE("A unit attached to another starts no order until it is let go", "[tick-phases]") {
    Phases w;
    const Unit& fac = w.make("fac", "fac", 1, 60.0f, 60.0f);
    Unit& tank = w.make("tank", "tank", 1, 60.0f, 60.0f);
    tank.set_max_speed(5.0f);
    tank.set_parent(fac.entity_id(), -1);
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {90.0f, 10.0f, 60.0f};
    tank.push_command(move, false);
    w.sim.tick();
    REQUIRE(tank.command_queue().size() == 1);
    CHECK_FALSE(tank.command_queue().front().begun);
    tank.clear_parent();
    w.sim.tick();
    REQUIRE(tank.command_queue().size() == 1);
    CHECK(tank.command_queue().front().begun);
}
