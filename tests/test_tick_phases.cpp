// A tick's phases in Moho's order (faf-re Sim::AdvanceBeat): each army's
// economy and weapons, then every unit's orders, then the Lua threads, then the
// entities, a unit's step in Unit::MotionTick's order.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/profiler.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/sim_sync.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/anim_cache.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/sca_parser.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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
                 "{BlueprintId = 'ship', Categories = {'NAVAL', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                 " Footprint = {SizeX = 1, SizeZ = 1}, Physics = {MotionType = 'RULEUMT_Water'}}",
                 "{BlueprintId = 'gen', CategoriesHash = {MOBILE = true, TECH1 = true},"
                 " Economy = {BuildTime = 10, BuildCostMass = 1, BuildCostEnergy = 1,"
                 " ProductionPerSecondEnergy = 5}, Defense = {MaxHealth = 10}}",
                 "{BlueprintId = 'healer', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100, RegenRate = 10}}",
                 "{BlueprintId = 'block', CategoriesHash = {STRUCTURE = true},"
                 " Footprint = {SizeX = 2, SizeZ = 2}, Physics = {MotionType = 'RULEUMT_None'},"
                 " Defense = {MaxHealth = 100}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(run("Plain = setmetatable({}, {__index = moho.unit_methods})"
                    " Plain.__index = Plain"
                    " function WaitTicks(n) coroutine.yield(n) end"
                    " EM = moho.entity_methods PM = moho.projectile_methods"));
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"tank", "fac", "t1", "ship", "gen", "healer", "block"}) {
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

    void brain(const char* global, int army) {
        lua_State* L = state.raw();
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, sim.get_army(army));
        lua_rawset(L, -3);
        lua_pushvalue(L, -1);
        sim.get_army(army)->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        lua_setglobal(L, global);
        REQUIRE(run(std::string("setmetatable(") + global + ", {__index = moho.aibrain_methods})"));
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

    void projectile_class(const char* bp, const std::string& fields) {
        REQUIRE(run("ProjClass = setmetatable({" + fields +
                    "}, {__index = moho.projectile_methods}) ProjClass.__index = ProjClass"));
        lua_State* L = state.raw();
        lua_pushstring(L, "__osc_proj_script_classes");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushstring(L, "__osc_proj_script_classes");
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);
        }
        lua_pushstring(L, bp);
        lua_getglobal(L, "ProjClass");
        lua_rawset(L, -3);
        lua_settop(L, 0);
    }

    void drop_shell() {
        REQUIRE(run("ForkThread(function()"
                    "   local shot = EM.CreateProjectile(shooter, '/shell', 0, 5, 0)"
                    "   PM.SetBallisticAcceleration(shot, 0)"
                    "   PM.SetVelocity(shot, 0, -35, 0)"
                    " end)"));
    }

    void fire_at_rival(Unit& rival) {
        osc::sim::CollisionShape box;
        box.type = osc::sim::CollisionShapeType::BOX;
        box.cy = 1.0f;
        box.sx = 1.0f;
        box.sy = 1.0f;
        box.sz = 1.0f;
        rival.set_collision_shape(box);
        REQUIRE(run("checked = 0 Plain.OnCollisionCheck = function(self, other)"
                    "   checked = GetGameTick() return true"
                    " end"));
        projectile_class("/shell", "OnImpact = function(self, kind, other)"
                                   "   hit = GetGameTick()"
                                   "   hit_x = EM.GetPosition(self)[1]"
                                   "   Damage(self, other, 10, 'Normal')"
                                   "   EM.Destroy(self)"
                                   " end");
        REQUIRE(run("hit = 0 hit_x = 0 seen = 0 seen_x = 0 ForkThread(function()"
                    "   shot = EM.CreateProjectile(shooter, '/shell', 0, 1, 0)"
                    "   PM.SetBallisticAcceleration(shot, 0)"
                    "   PM.SetVelocity(shot, 35, 0, 0)"
                    "   while not EM.BeenDestroyed(shot) do"
                    "     seen = GetGameTick() seen_x = EM.GetPosition(shot)[1]"
                    "     WaitTicks(1)"
                    "   end"
                    " end)"));
    }

    static void drive_east(Unit& u) {
        u.add_command_cap("RULEUCC_Move");
        u.set_max_speed(5.0f);
        u.set_orientation(osc::sim::euler_to_quat(1.5707964f, 0.0f, 0.0f));
        Unit::Drive drive;
        drive.max_accel = 50.0f;
        u.set_drive(drive);
        UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = {u.position().x + 30.0f, 10.0f, u.position().z};
        u.push_command(move, false);
    }

    void add_animation(const char* path) {
        osc::sim::SCAData sca;
        sca.num_frames = 2;
        sca.num_bones = 1;
        sca.duration = 1.0f;
        sca.bone_names.push_back("root");
        sca.parent_indices.push_back(-1);
        const osc::sim::Quaternion identity{0, 0, 0, 1};
        sca.frames.push_back({0.0f, {{{0, 0, 0}, identity}}});
        sca.frames.push_back({1.0f, {{{1, 0, 0}, identity}}});
        if (!sim.anim_cache()) {
            sim.set_anim_cache(std::make_unique<osc::sim::AnimCache>(nullptr));
        }
        sim.anim_cache()->inject(path, std::move(sca));
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

namespace {

osc::u32 seabed_tick(f32 stun_seconds, osc::u32 stun_tick, osc::u32* stunned_in) {
    Phases w;
    std::vector<osc::u16> heights(static_cast<size_t>(257) * 257);
    for (size_t z = 0; z <= 256; ++z) {
        for (size_t x = 0; x <= 256; ++x) {
            const f32 ground = 10.0f + (128.0f - static_cast<f32>(x)) * 0.05f;
            heights[z * 257 + x] = static_cast<osc::u16>(ground * 128.0f);
        }
    }
    osc::map::Heightmap hm(256, 256, 1.0f / 128.0f, std::move(heights));
    w.sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 10.7f, true));
    w.sim.build_pathfinding_grid();
    Unit& tank = w.make("tank", "tank", 1, 100.0f, 100.0f);
    tank.set_motion_type("RULEUMT_Amphibious");
    Phases::drive_east(tank);
    REQUIRE(w.run("stunned = 0"));
    for (int t = 0; t < 100; ++t) {
        if (stun_seconds > 0 && w.sim.tick_count() + 1 == stun_tick) {
            REQUIRE(w.run("ForkThread(function() tank:SetStunned(" + std::to_string(stun_seconds) +
                          ") stunned = GetGameTick() end)"));
        }
        w.sim.tick();
        if (tank.layer() == "Seabed") {
            *stunned_in = static_cast<osc::u32>(w.number("stunned"));
            return w.sim.tick_count();
        }
    }
    return 0;
}

} // namespace

TEST_CASE("A stun a Lua thread sets is counted down before that tick's motion step",
          "[tick-phases]") {
    osc::u32 none = 0;
    const osc::u32 shore = seabed_tick(0.0f, 0, &none);
    REQUIRE(shore > 2);
    const osc::u32 ticks = GENERATE(1u, 10u);
    CAPTURE(ticks);
    osc::u32 stunned = 0;
    const osc::u32 entered = seabed_tick(static_cast<f32>(ticks) / 10.0f, shore, &stunned);
    REQUIRE(stunned == shore);
    CHECK(entered == stunned + ticks - 1);
}

TEST_CASE("A ship its step finds on land is hurt in that step", "[tick-phases]") {
    Phases w;
    Unit& ship = w.make("ship", "ship", 1, 100.0f, 100.0f);
    REQUIRE(ship.layer() == "Water");
    w.sim.tick();
    REQUIRE(ship.health() == 100.0f);
    ship.set_velocity({5.0f, 0.0f, 0.0f});
    w.sim.tick();
    REQUIRE(ship.layer() == "Land");
    CHECK(ship.health() == Catch::Approx(99.0f));
}

TEST_CASE("An animator a motion event sets going plays in that tick's pose", "[tick-phases]") {
    Phases w;
    w.add_animation("/walk.sca");
    Unit& tank = w.make("tank", "tank", 1, 100.0f, 100.0f);
    REQUIRE(w.run("walk = CreateAnimator(tank):PlayAnim('/walk.sca', true):SetRate(0) started = 0"
                  " function tank:OnMotionHorzEventChange(new, old)"
                  "   if old == 'Stopped' then started = GetGameTick() walk:SetRate(1) end"
                  " end"));
    Phases::drive_east(tank);
    for (int t = 0; t < 10 && w.number("started") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("started") == static_cast<f32>(w.sim.tick_count()));
    REQUIRE(w.run("played = walk:GetAnimationFraction()"));
    CHECK(w.number("played") == Catch::Approx(0.1f));
}

TEST_CASE("An animator a silo's OnSiloBuildStart sets going plays in that tick's pose",
          "[tick-phases]") {
    Phases w;
    w.add_animation("/hatch.sca");
    Unit& silo = w.make("silo", "tank", 1, 100.0f, 100.0f);
    REQUIRE(w.run("__blueprints['/nuke'] = {Economy = {BuildTime = 100, BuildCostEnergy = 1,"
                  " BuildCostMass = 1}}"
                  " hatch = CreateAnimator(silo):PlayAnim('/hatch.sca'):SetRate(0) started = 0"
                  " function silo:OnSiloBuildStart(weapon) started = GetGameTick() hatch:SetRate(1)"
                  " end"));
    Weapon& launcher = w.arm(silo, nullptr);
    launcher.counted_projectile = true;
    launcher.nuke_weapon = true;
    launcher.max_projectile_storage = 1;
    launcher.projectile_bp_id = "/nuke";
    silo.set_build_rate(10.0f);
    silo.order_silo_build(true);
    for (int t = 0; t < 5 && w.number("started") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("started") == static_cast<f32>(w.sim.tick_count()));
    REQUIRE(w.run("played = hatch:GetAnimationFraction()"));
    CHECK(w.number("played") == Catch::Approx(0.1f));
}

TEST_CASE("A stats trigger that holds from the start fires on tick 11, in its army's turn",
          "[tick-phases]") {
    Phases w;
    w.brain("brain", 0);
    REQUIRE(w.run("fired = 0 forked = 0"
                  " function brain:OnStatsTrigger(name)"
                  "   fired = GetGameTick()"
                  "   ForkThread(function() forked = GetGameTick() end)"
                  " end"));
    w.sim.get_army(0)->add_stat_trigger({"Economy_Ratio_Mass", "Low", "LessThan", 0.1, ""});
    for (int t = 0; t < 15; ++t) {
        w.sim.tick();
    }
    CHECK(w.number("fired") == 11.0f);
    CHECK(w.number("forked") == 11.0f);
}

TEST_CASE("The unit-cap stats are counted after their army's triggers", "[tick-phases]") {
    Phases w;
    w.brain("brain", 0);
    REQUIRE(
        w.run("fired = 0 made = 0 function brain:OnStatsTrigger(name) fired = GetGameTick() end"));
    for (int t = 0; t < 12; ++t) {
        w.sim.tick();
    }
    const double before = w.sim.get_army(0)->get_stat("UnitCap_Current");
    w.sim.get_army(0)->add_stat_trigger({"UnitCap_Current", "More", "GreaterThan", before, ""});
    REQUIRE(
        w.run("ForkThread(function() made = GetGameTick() CreateUnit('tank', 1, 50, 10, 50) end)"));
    for (int t = 0; t < 5; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("made") > 0);
    CHECK(w.number("fired") == w.number("made") + 2);
}

TEST_CASE("Consumption a Lua thread turns on is paid at the next tick's economy and worked the "
          "tick after",
          "[tick-phases]") {
    Phases w;
    osc::sim::ArmyBrain& army = *w.sim.get_army(0);
    army.set_extra_storage(0.0, 1000.0);
    army.set_stored_resources(0.0, 500.0);
    Unit& eng = w.make("eng", "tank", 1, 100.0f, 100.0f);
    eng.economy().consumption_energy = 10.0;
    w.sim.tick();
    REQUIRE(army.economy().energy.stored == 500.0);
    REQUIRE(w.run(
        "consumed = {} ForkThread(function()"
        "   eng:SetConsumptionActive(true)"
        "   while true do WaitTicks(1) consumed[GetGameTick()] = eng:GetResourceConsumed() end"
        " end)"));
    w.sim.tick();
    const auto asked = static_cast<int>(w.sim.tick_count());
    CHECK(army.economy().energy.stored == 500.0);
    w.sim.tick();
    CHECK(army.economy().energy.stored == Catch::Approx(499.0));
    w.sim.tick();
    REQUIRE(w.run("next = consumed[" + std::to_string(asked + 1) + "] after = consumed[" +
                  std::to_string(asked + 2) + "]"));
    CHECK(w.number("next") == 0.0f);
    CHECK(w.number("after") == 1.0f);
}

TEST_CASE("A unit finished in a tick's orders is in its army's income from the next tick",
          "[tick-phases]") {
    Phases w;
    w.brain("brain", 0);
    Unit& fac = w.make("fac", "fac", 1, 60.0f, 60.0f);
    fac.economy().production_mass = 100.0;
    fac.economy().production_energy = 100.0;
    fac.economy().production_active = true;
    REQUIRE(w.run(
        "done = 0 income = {}"
        " function Plain:OnStopBeingBuilt() done = GetGameTick() end"
        " ForkThread(function()"
        "   while true do income[GetGameTick()] = brain:GetEconomyIncome('ENERGY') WaitTicks(1) end"
        " end)"));
    UnitCommand build;
    build.type = CommandType::BuildFactory;
    build.blueprint_id = "gen";
    fac.push_command(build, false);
    for (int t = 0; t < 30 && w.number("done") == 0; ++t) {
        w.sim.tick();
    }
    const auto done = static_cast<int>(w.number("done"));
    REQUIRE(done > 0);
    CHECK(w.sim.get_army(0)->economy().energy.income == 100.0);
    w.sim.tick();
    REQUIRE(w.run("was = income[" + std::to_string(done) + "] next = income[" +
                  std::to_string(done + 1) + "]"));
    CHECK(w.number("was") == Catch::Approx(10.0));
    CHECK(w.number("next") == Catch::Approx(10.5));
}

namespace {

std::pair<double, double> killed_in_its_step(const char* what, double stored_energy) {
    Phases w;
    w.brain("brain", 0);
    osc::sim::ArmyBrain& army = *w.sim.get_army(0);
    army.set_extra_storage(0.0, 100.0);
    army.set_stored_resources(0.0, stored_energy);
    Unit& ship = w.make("ship", "ship", 1, 100.0f, 100.0f);
    ship.economy().production_energy = 5.0;
    ship.economy().production_active = true;
    ship.economy().storage_energy = 100.0;
    ship.economy().consumption_energy = 1.0;
    ship.economy().consumption_active = true;
    w.sim.tick();
    REQUIRE(ship.layer() == "Water");
    ship.set_health(0.5f);
    ship.set_velocity({5.0f, 0.0f, 0.0f});
    REQUIRE(w.run(std::string("seen = {} ForkThread(function()"
                              "   while true do seen[GetGameTick()] = ") +
                  what + " WaitTicks(1) end end)"));
    const osc::u32 id = ship.entity_id();
    w.sim.tick();
    REQUIRE(w.sim.tick_count() == 2);
    const auto* left = w.sim.entity_registry().find(id);
    REQUIRE((!left || left->destroyed() || static_cast<const Unit*>(left)->is_dying()));
    w.sim.tick();
    REQUIRE(w.run("at2 = seen[2] at3 = seen[3]"));
    return {w.number("at2"), w.number("at3")};
}

} // namespace

TEST_CASE("A unit its own step kills produces nothing for the next economy", "[tick-phases]") {
    const auto [before, after] = killed_in_its_step("brain:GetEconomyIncome('ENERGY')", 0.0);
    CHECK(before == Catch::Approx(0.5));
    CHECK(after == 0.0);
}

TEST_CASE("A unit its own step kills takes its storage away by the next economy", "[tick-phases]") {
    const auto [before, after] = killed_in_its_step("brain:GetEconomyStoredRatio('ENERGY')", 50.0);
    CHECK(before == Catch::Approx(50.0 / 200.0).epsilon(0.05));
    CHECK(after == Catch::Approx(50.0 / 100.0).epsilon(0.05));
}

TEST_CASE("A unit its own step kills reads no ResourceConsumed from the next tick",
          "[tick-phases]") {
    const auto [before, after] = killed_in_its_step("ship:GetResourceConsumed()", 50.0);
    CHECK(before == 1.0);
    CHECK(after == 0.0);
}

TEST_CASE("An economy event a Lua thread makes takes its first step in its unit's step of the "
          "next tick",
          "[tick-phases]") {
    Phases w;
    osc::sim::ArmyBrain& army = *w.sim.get_army(0);
    army.set_extra_storage(0.0, 1000.0);
    army.set_stored_resources(0.0, 500.0);
    w.make("u", "tank", 1, 100.0f, 100.0f);
    w.sim.tick();
    REQUIRE(w.run(
        "made = 0 stepped = 0 waited = 0 ForkThread(function()"
        "   made = GetGameTick()"
        "   local e = CreateEconomyEvent(u, 10, 0, 0.1, function() stepped = GetGameTick() end)"
        "   WaitFor(e)"
        "   waited = GetGameTick()"
        " end)"));
    for (int t = 0; t < 5; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("made") > 0);
    CHECK(w.number("stepped") == w.number("made") + 1);
    CHECK(w.number("waited") == w.number("made") + 2);
}

TEST_CASE("Reclaim is banked in the order phase", "[tick-phases]") {
    Phases w;
    w.brain("brain", 0);
    Unit& eng = w.make("eng", "tank", 1, 100.0f, 100.0f);
    eng.economy().reclaim_mass = 10.0;
    w.sim.tick();
    REQUIRE(w.run(
        "seen = {} killed = 0 ForkThread(function()"
        "   killed = GetGameTick() eng:Kill()"
        "   while true do WaitTicks(1) seen[GetGameTick()] = brain:GetEconomyIncome('MASS') end"
        " end)"));
    for (int t = 0; t < 3; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("killed") > 0);
    REQUIRE(w.run("next = seen[killed + 1]"));
    CHECK(w.number("next") == Catch::Approx(1.0));
}

TEST_CASE("A resource a Lua thread gives is income at the next economy, stored through it",
          "[tick-phases]") {
    Phases w;
    w.brain("brain", 0);
    w.sim.get_army(0)->set_extra_storage(650.0, 4000.0);
    w.sim.tick();
    REQUIRE(
        w.run("given = 0 ForkThread(function()"
              "   given = GetGameTick() brain:GiveResource('Mass', 650)"
              "   now = brain:GetEconomyStored('MASS') WaitTicks(1)"
              "   stored = brain:GetEconomyStored('MASS') income = brain:GetEconomyIncome('MASS')"
              " end)"));
    for (int t = 0; t < 3; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("given") > 0);
    CHECK(w.number("now") == 0.0f);
    CHECK(w.number("stored") == Catch::Approx(650.0));
    CHECK(w.number("income") == Catch::Approx(650.0));
}

TEST_CASE("An army's overflow reaches a higher-index ally that tick, a lower-index one the next",
          "[tick-phases]") {
    const int giver = GENERATE(0, 1);
    CAPTURE(giver);
    const int taker = 1 - giver;
    Phases w;
    w.sim.set_team_share_overflow(true);
    w.sim.set_alliance(0, 1, osc::sim::Alliance::Ally);
    w.brain("taker", taker);
    for (int a = 0; a < 2; ++a) {
        w.sim.get_army(a)->set_extra_storage(200.0, 200.0);
    }
    w.sim.get_army(giver)->set_stored_resources(200.0, 0.0);
    Unit& producer = w.make("producer", "tank", giver + 1, 100.0f, 100.0f);
    producer.economy().production_mass = 1000.0;
    producer.economy().production_active = true;
    REQUIRE(w.run("arrived = 0 ForkThread(function()"
                  "   while arrived == 0 do"
                  "     if taker:GetEconomyStored('MASS') > 0 then arrived = GetGameTick() end"
                  "     WaitTicks(1)"
                  "   end"
                  " end)"));
    for (int t = 0; t < 6; ++t) {
        w.sim.tick();
    }
    CHECK(w.number("arrived") == static_cast<f32>(2 + giver));
}

TEST_CASE("A projectile's hit is found by one step and dealt at the start of its next",
          "[tick-phases]") {
    Phases w;
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    Unit& rival = w.make("rival", "tank", 2, 110.0f, 100.0f);
    w.fire_at_rival(rival);
    osc::u32 damaged = 0;
    for (int t = 0; t < 8 && w.number("hit") == 0; ++t) {
        w.sim.tick();
        if (damaged == 0 && rival.health() < rival.max_health()) {
            damaged = w.sim.tick_count();
        }
    }
    REQUIRE(w.number("checked") > 0);
    CHECK(w.number("hit") == w.number("checked") + 1);
    CHECK(static_cast<f32>(damaged) == w.number("hit"));
    CHECK(w.number("hit_x") == Catch::Approx(109.0f));
    CHECK(w.number("seen") == w.number("hit"));
    CHECK(w.number("seen_x") == Catch::Approx(w.number("hit_x")));
}

TEST_CASE("A scripted projectile's damage is its OnImpact's alone, however its weapon passed it",
          "[projectile][damage]") {
    const std::string passed =
        GENERATE(std::string("a table"), std::string("a metatable"), std::string("nothing"));
    CAPTURE(passed);
    Phases w;
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    Unit& rival = w.make("rival", "tank", 2, 110.0f, 100.0f);
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.cy = 1.0f;
    box.sx = 1.0f;
    box.sy = 1.0f;
    box.sz = 1.0f;
    rival.set_collision_shape(box);
    REQUIRE(w.run("Plain.OnCollisionCheck = function(self, other) return true end"));
    w.projectile_class("/shell", "OnImpact = function(self, kind, other)"
                                 "   local data = self.DamageData"
                                 "   if data and data.DamageAmount and other then"
                                 "     Damage(self, other, data.DamageAmount, 'Normal')"
                                 "   end"
                                 "   hit = GetGameTick()"
                                 "   EM.Destroy(self)"
                                 " end");
    std::string pass;
    if (passed == "a table") {
        pass = "shot.DamageData = {DamageAmount = 10}";
    } else if (passed == "a metatable") {
        pass = "local data = {DamageAmount = 10} data.__index = data"
               " shot.DamageData = setmetatable({}, data)";
    }
    REQUIRE(w.run("hit = 0 ForkThread(function()"
                  "   shot = EM.CreateProjectile(shooter, '/shell', 0, 1, 0)"
                  "   PM.SetDamage(shot, 10)"
                  "   PM.SetBallisticAcceleration(shot, 0)"
                  "   PM.SetVelocity(shot, 35, 0, 0) " +
                  pass + " end)"));
    const f32 full = rival.health();
    for (int t = 0; t < 8 && w.number("hit") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("hit") > 0);
    CHECK(full - rival.health() == Catch::Approx(passed == "nothing" ? 0.0f : 10.0f));
}

TEST_CASE("A shot from a weapon with no script takes the engine's damage once",
          "[projectile][damage]") {
    const bool scripted = GENERATE(false, true);
    CAPTURE(scripted);
    Phases w;
    Unit& shooter = w.make("shooter", "tank", 1, 100.0f, 100.0f);
    Unit& rival = w.make("rival", "tank", 2, 110.0f, 100.0f);
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.cy = 1.0f;
    box.sx = 1.0f;
    box.sy = 1.0f;
    box.sz = 1.0f;
    rival.set_collision_shape(box);
    REQUIRE(w.run("hits = 0 Plain.OnCollisionCheck = function(self, other) return true end"));
    if (scripted) {
        w.projectile_class("/shell", "OnImpact = function(self, kind, other)"
                                     "   local data = self.DamageData"
                                     "   if data and data.DamageAmount and other then"
                                     "     Damage(self, other, data.DamageAmount, 'Normal')"
                                     "   end"
                                     "   hits = hits + 1"
                                     "   EM.Destroy(self)"
                                     " end");
    }
    Weapon& gun = w.arm(shooter, nullptr);
    gun.muzzle_velocity = 35.0f;
    const f32 full = rival.health();
    for (int t = 0; t < 30 && rival.health() == full; ++t) {
        w.sim.tick();
    }
    CHECK(full - rival.health() == Catch::Approx(10.0f));
    if (scripted) {
        CHECK(w.number("hits") == 1.0f);
    }
}

TEST_CASE("The snapshot of the tick that finds a hit holds the projectile at the impact point",
          "[tick-phases]") {
    Phases w;
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    Unit& rival = w.make("rival", "tank", 2, 110.0f, 100.0f);
    w.fire_at_rival(rival);
    osc::u32 shot_id = 0;
    for (int t = 0; t < 8 && w.number("checked") == 0; ++t) {
        w.sim.tick();
        w.sim.entity_registry().for_each([&](osc::sim::Entity& e) {
            if (e.is_projectile()) {
                shot_id = e.entity_id();
            }
        });
    }
    REQUIRE(w.number("checked") > 0);
    REQUIRE(shot_id != 0);
    osc::sim::WorldSnapshot snap;
    osc::sim::capture_world(w.sim, snap);
    const osc::sim::EntityRecord* shot = snap.find(shot_id);
    REQUIRE(shot);
    CHECK(shot->position.x == Catch::Approx(109.0f));
}

TEST_CASE("A fragment made in its parent's OnImpact moves in that tick", "[tick-phases]") {
    Phases w;
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    w.projectile_class("/shell", "OnImpact = function(self, kind)"
                                 "   local c = PM.CreateChildProjectile(self, '/frag')"
                                 "   PM.SetBallisticAcceleration(c, 0)"
                                 "   PM.SetCollideSurface(c, false)"
                                 "   PM.SetVelocity(c, 0, 20, 0)"
                                 "   made = GetGameTick()"
                                 "   made_y = EM.GetPosition(c)[2]"
                                 "   EM.Destroy(self)"
                                 " end");
    REQUIRE(w.run("made = 0 made_y = 0"));
    w.drop_shell();
    for (int t = 0; t < 8 && w.number("made") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("made") > 0);
    const osc::sim::Entity* frag = nullptr;
    w.sim.entity_registry().for_each([&](osc::sim::Entity& e) {
        if (e.is_projectile() && e.blueprint_id() == "/frag") {
            frag = &e;
        }
    });
    REQUIRE(frag);
    CHECK(frag->position().y == Catch::Approx(w.number("made_y") + 2.0f));
}

TEST_CASE("A unit made during the entity walk takes its step in that tick", "[tick-phases]") {
    Phases w;
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    w.projectile_class("/shell", "OnImpact = function(self, kind)"
                                 "   made = GetGameTick()"
                                 "   healer = CreateUnit('healer', 1, 150, 10, 150)"
                                 "   EM.SetHealth(healer, healer, 50)"
                                 "   EM.Destroy(self)"
                                 " end");
    REQUIRE(w.run("made = 0"));
    w.drop_shell();
    for (int t = 0; t < 8 && w.number("made") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("made") > 0);
    lua_State* L = w.state.raw();
    lua_getglobal(L, "healer");
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    const auto* healer = static_cast<const Unit*>(lua_touserdata(L, -1));
    lua_settop(L, 0);
    REQUIRE(healer);
    CHECK(healer->health() == Catch::Approx(51.0f));
}

TEST_CASE("SetLifetime(0.5) from OnLostTarget leaves a projectile six steps", "[tick-phases]") {
    Phases w;
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    w.make("rival", "tank", 2, 110.0f, 100.0f);
    w.projectile_class("/missile", "OnLostTarget = function(self)"
                                   "   lost = GetGameTick()"
                                   "   PM.SetLifetime(self, 0.5)"
                                   " end,"
                                   " OnImpact = function(self, kind)"
                                   "   hit = GetGameTick() hit_kind = kind"
                                   "   hit_x = EM.GetPosition(self)[1]"
                                   "   EM.Destroy(self)"
                                   " end");
    REQUIRE(w.run("lost = 0 hit = 0 hit_x = 0 hit_kind = '' ForkThread(function()"
                  "   local shot = EM.CreateProjectile(shooter, '/missile', 0, 50, 0)"
                  "   PM.SetBallisticAcceleration(shot, 0)"
                  "   PM.SetVelocityAlign(shot, false)"
                  "   PM.SetVelocity(shot, 10, 0, 0)"
                  "   PM.SetNewTarget(shot, rival)"
                  "   PM.TrackTarget(shot, true)"
                  "   WaitTicks(3)"
                  "   EM.Destroy(rival)"
                  " end)"));
    osc::u32 shot_id = 0;
    std::vector<f32> xs;
    for (int t = 0; t < 20 && w.number("hit") == 0; ++t) {
        w.sim.tick();
        w.sim.entity_registry().for_each([&](osc::sim::Entity& e) {
            if (e.is_projectile()) {
                shot_id = e.entity_id();
            }
        });
        const osc::sim::Entity* shot = w.sim.entity_registry().find(shot_id);
        xs.push_back(shot ? shot->position().x : 0.0f);
    }
    REQUIRE(w.number("lost") > 1);
    REQUIRE(w.number("hit") > 0);
    CHECK(w.text("hit_kind") == "Air");
    CHECK(w.number("hit") == w.number("lost") + 6);
    const f32 before_lost = xs[static_cast<size_t>(w.number("lost")) - 2];
    CHECK(w.number("hit_x") - before_lost == Catch::Approx(6.0f));
}

TEST_CASE("A projectile's sweep finds a shield a tenth of a step past the step's end",
          "[tick-phases]") {
    Phases w;
    auto bubble = std::make_unique<osc::sim::Shield>();
    bubble->set_position({130.0f, 30.0f, 100.0f});
    osc::sim::CollisionShape sphere;
    sphere.type = osc::sim::CollisionShapeType::SPHERE;
    sphere.sx = 13.0f;
    bubble->set_collision_shape(sphere);
    w.sim.entity_registry().register_entity(std::move(bubble));
    auto made = std::make_unique<osc::sim::Projectile>();
    made->set_position({102.825f, 30.0f, 100.0f});
    made->velocity = {35.0f, 0.0f, 0.0f};
    const osc::u32 shot_id = w.sim.entity_registry().register_entity(std::move(made));
    for (int t = 0; t < 4; ++t) {
        w.sim.tick();
    }
    const osc::sim::Entity* shot = w.sim.entity_registry().find(shot_id);
    REQUIRE(shot);
    CHECK(shot->position().x == Catch::Approx(117.0f).margin(0.001f));
}

namespace {

void give_box(Unit& u) {
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.cy = 1.0f;
    box.sx = 1.0f;
    box.sy = 1.0f;
    box.sz = 1.0f;
    u.set_collision_shape(box);
}

void watch_rival(Phases& w) {
    REQUIRE(w.run("rival_x = {} ForkThread(function()"
                  "   while true do"
                  "     rival_x[GetGameTick()] = EM.GetPosition(rival)[1]"
                  "     WaitTicks(1)"
                  "   end"
                  " end)"));
}

struct Meeting {
    f32 checked = 0;
    f32 hit_x = 0;
    f32 rival_x = 0;
};

Meeting shell_meets_tank(bool tank_first) {
    Phases w;
    w.make("shooter", "tank", 1, 140.0f, 100.0f);
    REQUIRE(w.run("checked = 0 hit_x = 0 Plain.OnCollisionCheck = function(self, other)"
                  "   checked = GetGameTick() return true"
                  " end"));
    w.projectile_class("/shell", "OnImpact = function(self, kind, other)"
                                 "   hit_x = EM.GetPosition(self)[1]"
                                 "   EM.Destroy(self)"
                                 " end");
    const auto shoot = [&] {
        REQUIRE(w.run("shot = EM.CreateProjectile(shooter, '/shell', 0, 1, 0)"
                      " PM.SetBallisticAcceleration(shot, 0) PM.SetVelocity(shot, -35, 0, 0)"));
    };
    if (!tank_first) {
        shoot();
    }
    Unit& tank = w.make("rival", "tank", 2, 100.0f, 100.0f);
    if (tank_first) {
        shoot();
    }
    give_box(tank);
    Phases::drive_east(tank);
    watch_rival(w);
    for (int t = 0; t < 20 && w.number("hit_x") == 0; ++t) {
        w.sim.tick();
    }
    Meeting m;
    m.checked = w.number("checked");
    m.hit_x = w.number("hit_x");
    REQUIRE(w.run("seen_x = rival_x[checked] or 0"));
    m.rival_x = w.number("seen_x");
    return m;
}

f32 blast_by_tank(bool tank_first) {
    Phases w;
    w.make("shooter", "tank", 1, 140.0f, 100.0f);
    REQUIRE(w.run("damaged = 0 Plain.OnDamage = function(self) damaged = GetGameTick() end"));
    w.projectile_class("/shell", "OnImpact = function(self, kind)"
                                 "   local at = EM.GetPosition(self)"
                                 "   local r = math.abs(rival_x[GetGameTick()] - at[1]) + 0.25"
                                 "   DamageArea(self, at, r, 10, 'Normal', false)"
                                 "   EM.Destroy(self)"
                                 " end");
    const auto shoot = [&] {
        REQUIRE(w.run("shot = EM.CreateProjectile(shooter, '/shell', -42, 5, 0)"
                      " PM.SetBallisticAcceleration(shot, 0) PM.SetVelocity(shot, 0, -35, 0)"));
    };
    if (!tank_first) {
        shoot();
    }
    Unit& tank = w.make("rival", "tank", 2, 100.0f, 100.0f);
    if (tank_first) {
        shoot();
    }
    tank.set_collision_shape({});
    Phases::drive_east(tank);
    watch_rival(w);
    for (int t = 0; t < 8; ++t) {
        w.sim.tick();
    }
    return w.number("damaged");
}

} // namespace

TEST_CASE("A projectile meets a moving unit where the unit stood as the tick began",
          "[tick-phases][committed]") {
    const bool tank_first = GENERATE(true, false);
    CAPTURE(tank_first);
    const Meeting m = shell_meets_tank(tank_first);
    REQUIRE(m.checked > 0);
    CHECK(m.hit_x == Catch::Approx(m.rival_x + 1.0f));
}

TEST_CASE("A shot meets its target and a blast reaches a unit alike whichever was made first",
          "[tick-phases][committed]") {
    const Meeting a = shell_meets_tank(true);
    const Meeting b = shell_meets_tank(false);
    REQUIRE(a.checked > 0);
    CHECK(a.checked == b.checked);
    CHECK(a.hit_x == Catch::Approx(b.hit_x));
    CHECK(blast_by_tank(true) > 0);
    CHECK(blast_by_tank(true) == blast_by_tank(false));
}

TEST_CASE("A unit Warp moves is found at its new place by GetUnitsInRect in the same thread",
          "[tick-phases][committed]") {
    Phases w;
    w.sim.build_spatial_grid();
    Unit& tank = w.make("rival", "tank", 2, 100.0f, 100.0f);
    give_box(tank);
    w.sim.tick();
    REQUIRE(w.run("there = 0 here = 0 ForkThread(function()"
                  "   Warp(rival, {150, 10, 150})"
                  "   there = table.getn(GetUnitsInRect(148, 148, 152, 152) or {})"
                  "   here = table.getn(GetUnitsInRect(98, 98, 102, 102) or {})"
                  " end)"));
    w.sim.tick();
    CHECK(w.number("there") == 1.0f);
    CHECK(w.number("here") == 0.0f);
}

TEST_CASE("SetPosition from Lua moves an entity when the tick's moves are committed or at once",
          "[tick-phases][committed]") {
    const bool immediate = GENERATE(false, true);
    CAPTURE(immediate);
    Phases w;
    w.make("site", "fac", 1, 100.0f, 100.0f);
    w.sim.tick();
    REQUIRE(w.run(std::string("same = 0 next = 0 ForkThread(function()"
                              "   EM.SetPosition(site, {120, 10, 100}, ") +
                  (immediate ? "true" : "false") +
                  ")"
                  "   same = EM.GetPosition(site)[1]"
                  "   WaitTicks(1)"
                  "   next = EM.GetPosition(site)[1]"
                  " end)"));
    w.sim.tick();
    w.sim.tick();
    CHECK(w.number("same") == Catch::Approx(immediate ? 120.0f : 100.0f));
    CHECK(w.number("next") == Catch::Approx(120.0f));
}

TEST_CASE("A unit's velocity is its move between the last two commits, or its own motion's "
          "off the air layer",
          "[tick-phases][committed]") {
    Phases w;
    const Unit& site = w.make("site", "fac", 1, 100.0f, 100.0f);
    Unit& rider = w.make("rider", "tank", 1, 110.0f, 100.0f);
    w.sim.tick();
    rider.set_parent(site.entity_id(), -1);
    REQUIRE(w.run("moved = {} ForkThread(function()"
                  "   EM.SetPosition(site, {101, 10, 100})"
                  "   for i = 1, 2 do"
                  "     WaitTicks(1)"
                  "     moved[i] = moho.unit_methods.GetVelocity(site)"
                  "   end"
                  " end)"));
    for (int t = 0; t < 3; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.run("moved1 = moved[1] moved2 = moved[2]"));
    CHECK(w.number("moved1") == Catch::Approx(1.0f));
    CHECK(w.number("moved2") == Catch::Approx(0.0f));
    CHECK(rider.position().x == Catch::Approx(101.0f));
    CHECK(rider.velocity().x == 0.0f);
}

namespace {

int fits_tank(lua_State* L) {
    const auto* sim =
        static_cast<const osc::sim::SimState*>(lua_touserdata(L, lua_upvalueindex(1)));
    const auto* fp =
        static_cast<const osc::blueprints::Footprint*>(lua_touserdata(L, lua_upvalueindex(2)));
    const auto x = static_cast<f32>(lua_tonumber(L, 1));
    const auto z = static_cast<f32>(lua_tonumber(L, 2));
    lua_pushboolean(L, sim->footprint_fits_at(*fp, x, z) != 0 ? 1 : 0);
    return 1;
}

} // namespace

TEST_CASE("A unit a thread destroys is found and takes its step, then hears OnDestroy as the tick "
          "ends",
          "[tick-phases][death]") {
    Phases w;
    Unit& victim = w.make("victim", "tank", 1, 100.0f, 100.0f);
    Unit& later = w.make("later", "tank", 1, 100.0f, 120.0f);
    Phases::drive_east(victim);
    Phases::drive_east(later);
    const osc::u32 victim_id = victim.entity_id();
    REQUIRE(w.run("log = {} victim.tag = 'victim' later.tag = 'later'"
                  " Plain.OnMotionHorzEventChange = function(self)"
                  "   if GetEntityById(" +
                  std::to_string(victim_id) +
                  ") == victim then"
                  "     table.insert(log, self.tag)"
                  "   end"
                  " end"
                  " Plain.OnDestroy = function(self)"
                  "   table.insert(log, 'OnDestroy ' .. self.tag)"
                  " end"
                  " ForkThread(function()"
                  "   EM.Destroy(victim)"
                  "   been = EM.BeenDestroyed(victim) gone = IsDestroyed(victim)"
                  "   table.insert(log, 'Destroy')"
                  " end)"));
    w.sim.tick();
    CHECK(w.run("assert(been and not gone)"));
    CHECK(w.run("assert(table.concat(log, ', ') == 'Destroy, victim, later, OnDestroy victim',"
                " table.concat(log, ', '))"));
    CHECK(w.sim.entity_registry().find(victim_id) == nullptr);
}

TEST_CASE("A salvo with a muzzle delay stops at the end of its unit's death tick",
          "[tick-phases][death]") {
    const bool on_a_shot = GENERATE(false, true);
    CAPTURE(on_a_shot);
    Phases w;
    Unit& shooter = w.make("shooter", "tank", 1, 100.0f, 100.0f);
    w.make("rival", "tank", 2, 115.0f, 100.0f);
    REQUIRE(w.run("shots = {} killed = 0"
                  " Plain.OnKilled = function(self) killed = GetGameTick() end"
                  " gun = {OnFire = function(self)"
                  "   if self.salvo then return end"
                  "   self.salvo = ForkThread(function()"
                  "     for i = 1, 10 do table.insert(shots, GetGameTick()) WaitTicks(3) end"
                  "   end)"
                  " end,"
                  " OnDestroy = function(self) KillThread(self.salvo) end}"));
    w.arm(shooter, "gun");
    REQUIRE(w.run(std::string("ForkThread(function()"
                              "   while table.getn(shots) < 2 do WaitTicks(1) end") +
                  (on_a_shot ? "   WaitTicks(1)" : "") +
                  "   shooter:Kill()"
                  " end)"));
    for (int t = 0; t < 60; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("killed") > 0);
    REQUIRE(w.run("after = 0 for _, t in shots do if t >= killed then after = after + 1 end end"
                  " fired = table.concat(shots, ' ')"));
    CAPTURE(w.number("killed"), w.text("fired"));
    CHECK(w.number("after") == (on_a_shot ? 1.0f : 0.0f));
}

TEST_CASE("Ground a destroyed structure stood on is free from the next tick",
          "[tick-phases][death]") {
    Phases w;
    const Unit& tank = w.make("tank", "tank", 1, 60.0f, 60.0f);
    const osc::blueprints::Footprint footprint = tank.footprint();
    w.make("block", "block", 1, 101.0f, 101.0f);
    lua_State* L = w.state.raw();
    lua_pushlightuserdata(L, &w.sim);
    lua_pushlightuserdata(L, const_cast<osc::blueprints::Footprint*>(&footprint));
    lua_pushcclosure(L, fits_tank, 2);
    lua_setglobal(L, "fits");
    w.sim.tick();
    REQUIRE_FALSE(w.run("assert(fits(101, 101))"));
    REQUIRE(w.run("same = 0 next = 0"
                  " ForkThread(function() EM.Destroy(block) end)"
                  " ForkThread(function()"
                  "   same = fits(101, 101) and 1 or 0"
                  "   WaitTicks(1)"
                  "   next = fits(101, 101) and 1 or 0"
                  " end)"));
    w.sim.tick();
    w.sim.tick();
    CHECK(w.number("same") == 0.0f);
    CHECK(w.number("next") == 1.0f);
}

TEST_CASE("Kill takes a unit's adjacency and cargo away before its OnKilled, and counts it after",
          "[tick-phases][death]") {
    Phases w;
    w.brain("brain", 0);
    Unit& a = w.make("a", "block", 1, 101.0f, 101.0f);
    Unit& b = w.make("b", "block", 1, 103.0f, 101.0f);
    a.add_adjacent(b.entity_id());
    b.add_adjacent(a.entity_id());
    Unit& carrier = w.make("t", "tank", 1, 120.0f, 100.0f);
    Unit& cargo = w.make("c", "tank", 1, 120.0f, 100.0f);
    carrier.add_cargo(cargo.entity_id());
    cargo.set_transport_id(carrier.entity_id());
    cargo.set_unit_state("Attached", true);
    REQUIRE(w.run("log = {} a.tag = 'a' b.tag = 'b' t.tag = 't' c.tag = 'c'"
                  " Plain.CheckCanBeKilled = function(self) return true end"
                  " Plain.OnNotAdjacentTo = function(self, other)"
                  "   table.insert(log, self.tag .. ' not ' .. other.tag)"
                  " end"
                  " Plain.OnKilled = function(self)"
                  "   table.insert(log, 'killed ' .. self.tag .. ' '"
                  "     .. brain:GetArmyStat('Units_Killed', 0).Value)"
                  " end"
                  " ForkThread(function() a:Kill() t:Kill() end)"));
    w.sim.tick();
    CHECK(w.run("assert(table.concat(log, ', ') =="
                " 'a not b, b not a, killed a 0, killed c 1, killed t 2',"
                " table.concat(log, ', '))"));
}

TEST_CASE("A killed unit's weapons hear OnDestroy at the end of its death tick and are gone from "
          "the next",
          "[tick-phases][death]") {
    Phases w;
    Unit& shooter = w.make("shooter", "tank", 1, 100.0f, 100.0f);
    REQUIRE(w.run("log = {} counts = {}"
                  " Plain.OnKilled = function(self)"
                  "   ForkThread(function() table.insert(log, 'DeathThread ' .. GetGameTick()) end)"
                  " end"
                  " gun = {OnDestroy = function(self)"
                  "   table.insert(log, 'weapon OnDestroy ' .. GetGameTick())"
                  " end}"));
    w.arm(shooter, "gun");
    REQUIRE(w.run("killed = 0 ForkThread(function()"
                  "   WaitTicks(2)"
                  "   shooter:Kill()"
                  "   killed = GetGameTick()"
                  "   table.insert(counts, moho.unit_methods.GetWeaponCount(shooter))"
                  "   WaitTicks(1)"
                  "   table.insert(counts, moho.unit_methods.GetWeaponCount(shooter))"
                  " end)"));
    for (int t = 0; t < 5; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("killed") > 0);
    CHECK(w.run("local k = killed assert(table.concat(log, ', ') =="
                " 'DeathThread ' .. k .. ', weapon OnDestroy ' .. k, table.concat(log, ', '))"));
    CHECK(w.run("assert(table.concat(counts, ', ') == '1, 0', table.concat(counts, ', '))"));
}

namespace {

void script_entity(Phases& w, const char* global) {
    REQUIRE(w.run(std::string(global) + " = setmetatable({}, {__index = EM}) _c_CreateEntity(" +
                  global + ", {})"));
}

f32 lua_x(Phases& w, const char* global) {
    REQUIRE(w.run(std::string("at_x = EM.GetPosition(") + global + ")[1]"));
    return w.number("at_x");
}

void lua_handle(Phases& w, const char* global, osc::sim::Entity& e) {
    lua_State* L = w.state.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &e);
    lua_rawset(L, -3);
    lua_pushvalue(L, -1);
    e.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    lua_setglobal(L, global);
}

} // namespace

TEST_CASE("A chain of attached entities follows its root the tick the root moves, whichever was "
          "made first",
          "[tick-phases][walk]") {
    const bool leaf_first = GENERATE(false, true);
    Phases w;
    if (leaf_first) {
        script_entity(w, "leaf");
        w.make("mid", "tank", 1, 100.0f, 100.0f);
        w.make("root", "fac", 1, 100.0f, 100.0f);
    } else {
        w.make("root", "fac", 1, 100.0f, 100.0f);
        w.make("mid", "tank", 1, 100.0f, 100.0f);
        script_entity(w, "leaf");
    }
    REQUIRE(w.run("EM.AttachTo(mid, root, -1) EM.AttachTo(leaf, mid, -1)"));
    w.sim.tick();
    REQUIRE(w.run("EM.SetPosition(root, {104, 10, 100})"));
    w.sim.tick();
    INFO("leaf_first " << leaf_first);
    CHECK(lua_x(w, "mid") == Catch::Approx(104.0f));
    CHECK(lua_x(w, "leaf") == Catch::Approx(104.0f));
}

TEST_CASE("A factory's frame, and an entity on it, follow the factory the tick it moves",
          "[tick-phases][walk]") {
    Phases w;
    Unit& fac = w.make("fac", "fac", 1, 60.0f, 60.0f);
    REQUIRE(w.run("framed = 0"
                  " function fac:OnStartBuild(u, order)"
                  "   frame = u framed = GetGameTick()"
                  "   EM.AttachBoneTo(u, -2, self, -1)"
                  "   fx = setmetatable({}, {__index = EM}) _c_CreateEntity(fx, {})"
                  "   EM.AttachTo(fx, u, -1)"
                  " end"));
    UnitCommand build;
    build.type = CommandType::BuildFactory;
    build.blueprint_id = "t1";
    fac.push_command(build, false);
    for (int t = 0; t < 10 && w.number("framed") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("framed") > 0);
    w.sim.tick();
    REQUIRE(w.run("EM.SetPosition(fac, {64, 10, 60})"));
    w.sim.tick();
    CHECK(lua_x(w, "frame") == Catch::Approx(64.0f));
    CHECK(lua_x(w, "fx") == Catch::Approx(64.0f));
}

TEST_CASE("Cargo and a carrier's stored unit take their own step as they ride: their stun runs "
          "down and they regenerate",
          "[tick-phases][walk]") {
    const bool stored = GENERATE(false, true);
    Phases w;
    Unit& rider = w.make("rider", "healer", 1, 100.0f, 100.0f);
    Unit& carrier = w.make("carrier", "tank", 1, 100.0f, 100.0f);
    if (stored) {
        carrier.add_to_storage(rider, w.sim.entity_registry(), w.state.raw());
    } else {
        rider.attach_to_transport(&carrier, w.sim.entity_registry(), w.state.raw());
    }
    w.sim.tick();
    rider.set_health(50.0f);
    rider.set_stunned(0.5);
    REQUIRE(w.run("EM.SetPosition(carrier, {104, 10, 100})"));
    w.sim.tick();
    INFO("stored " << stored);
    CHECK(rider.position().x == Catch::Approx(104.0f));
    CHECK(rider.health() == Catch::Approx(51.0f));
    CHECK(rider.stun_ticks() == 4);
}

TEST_CASE("A tree a projectile hits leans in the tick of the hit", "[tick-phases][walk]") {
    Phases w;
    auto owned = std::make_unique<osc::sim::Prop>();
    osc::sim::Prop& tree = *owned;
    tree.set_position({110.0f, 10.0f, 100.0f});
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.cy = 1.0f;
    box.sx = 1.0f;
    box.sy = 1.0f;
    box.sz = 1.0f;
    tree.set_collision_shape(box);
    w.sim.entity_registry().register_entity(std::move(owned));
    lua_handle(w, "tree", tree);
    w.make("shooter", "tank", 1, 100.0f, 100.0f);
    w.projectile_class("/shell", "OnImpact = function(self, kind, other)"
                                 "   hit = GetGameTick() hit_kind = kind"
                                 "   moho.prop_methods.FallDown(other):Whack(1, 0, 0, 0.25, true)"
                                 "   EM.Destroy(self)"
                                 " end");
    REQUIRE(w.run("hit = 0 hit_kind = ''"
                  " shot = EM.CreateProjectile(shooter, '/shell', 0, 1, 0)"
                  " PM.SetBallisticAcceleration(shot, 0) PM.SetVelocity(shot, 35, 0, 0)"));
    for (int t = 0; t < 20 && w.number("hit") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("hit") > 0);
    REQUIRE(w.text("hit_kind") == "Prop");
    const osc::sim::Vector3 up = osc::sim::quat_rotate(tree.orientation(), {0.0f, 1.0f, 0.0f});
    CHECK(up.x == Catch::Approx(std::sin(0.25f)));
}

TEST_CASE("A beam deals its damage at its place in the entity walk, after its unit's step",
          "[tick-phases][walk]") {
    const bool beam_first = GENERATE(false, true);
    Phases w;
    Unit& launcher = w.make("launcher", "tank", 1, 100.0f, 100.0f);
    Weapon& gun = w.arm(launcher, nullptr);
    w.make("shooter", "tank", 1, 90.0f, 110.0f);
    give_box(w.make("target", "tank", 1, 100.0f, 110.0f));
    lua_State* L = w.state.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &gun);
    lua_rawset(L, -3);
    lua_pushstring(L, "unit");
    lua_getglobal(L, "launcher");
    lua_rawset(L, -3);
    lua_setglobal(L, "gun");
    w.projectile_class("/shell", "OnImpact = function(self, kind, other)"
                                 "   shell = GetGameTick() after_beam = beamed == shell"
                                 "   EM.Destroy(self)"
                                 " end");
    const auto beam = [&] {
        REQUIRE(w.run("beamed = 0 beam = setmetatable({OnImpact = function(self, kind, other)"
                      "   beamed = GetGameTick()"
                      " end}, {__index = moho.CollisionBeamEntity})"
                      " moho.CollisionBeamEntity.__init(beam, {Weapon = gun,"
                      "   CollisionCheckInterval = 0})"
                      " moho.CollisionBeamEntity.Enable(beam)"));
    };
    const auto shoot = [&] {
        REQUIRE(w.run("shell = 0 after_beam = false"
                      " shot = EM.CreateProjectile(shooter, '/shell', 0, 1, 0)"
                      " PM.SetBallisticAcceleration(shot, 0) PM.SetVelocity(shot, 35, 0, 0)"));
    };
    if (beam_first) {
        beam();
        shoot();
    } else {
        shoot();
        beam();
    }
    for (int t = 0; t < 20 && w.number("shell") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("shell") > 0);
    REQUIRE(w.number("beamed") == w.number("shell"));
    INFO("beam_first " << beam_first);
    CHECK(w.run(std::string("assert(after_beam == ") + (beam_first ? "true" : "false") + ")"));
}

TEST_CASE("A downed aircraft's OnImpact runs in its own step: a shot it makes moves that tick",
          "[tick-phases][walk]") {
    Phases w;
    Unit& plane = w.make("plane", "tank", 1, 100.0f, 100.0f);
    REQUIRE(w.run("made = 0 made_y = 0"
                  " function plane:OnImpact(kind)"
                  "   made = GetGameTick()"
                  "   shot = EM.CreateProjectile(self, '/shell', 0, 5, 0)"
                  "   PM.SetBallisticAcceleration(shot, 0)"
                  "   PM.SetCollideSurface(shot, false)"
                  "   PM.SetVelocity(shot, 0, 20, 0)"
                  "   made_y = EM.GetPosition(shot)[2]"
                  " end"));
    w.projectile_class("/shell", "");
    plane.set_layer("Air");
    plane.set_position({100.0f, 11.0f, 100.0f});
    plane.place_now();
    plane.begin_dying();
    for (int t = 0; t < 20 && w.number("made") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("made") > 0);
    REQUIRE(w.run("shot_y = EM.GetPosition(shot)[2]"));
    CHECK(w.number("shot_y") == Catch::Approx(w.number("made_y") + 2.0f));
}

TEST_CASE("An entity attached to a unit pushed out of another's way follows the push",
          "[tick-phases][walk]") {
    Phases w;
    Unit& still = w.make("still", "tank", 1, 100.0f, 100.0f);
    Unit& pushed = w.make("pushed", "tank", 1, 100.5f, 100.0f);
    for (Unit* u : {&still, &pushed}) {
        u->set_max_speed(5.0f);
        give_box(*u);
    }
    script_entity(w, "rider");
    REQUIRE(w.run("EM.AttachTo(rider, pushed, -1)"));
    w.sim.tick();
    w.sim.tick();
    REQUIRE(pushed.position().x != Catch::Approx(100.5f));
    CHECK(lua_x(w, "rider") == Catch::Approx(pushed.position().x));
}

namespace {

struct Recon : Phases {
    int armies;

    explicit Recon(int count) : armies(count) {
        for (int a = 2; a < count; ++a) {
            const std::string name = "ARMY_" + std::to_string(a + 1);
            sim.add_army(name, name);
        }
        sim.set_fog_of_war("explored");
        REQUIRE(run("gone = {} BlipClass = setmetatable({OnDestroy = function(self)"
                    "   table.insert(gone, GetGameTick())"
                    " end}, {__index = moho.blip_methods}) BlipClass.__index = BlipClass"
                    " function import(path) return {Blip = BlipClass} end"));
        for (int a = 0; a < count; ++a) {
            brain(("B" + std::to_string(a)).c_str(), a);
        }
    }

    Unit& eye(const char* global, int army, f32 x, f32 z, const char* intel, f32 radius) {
        Unit& u = make(global, "tank", army + 1, x, z);
        u.add_intel(intel, radius);
        u.set_intel_radius(intel, radius);
        u.enable_intel(intel);
        return u;
    }

    int turn_after(int tick, int army) const {
        const int from = std::max(tick, 1);
        return from + ((army - from) % armies + armies) % armies;
    }

    void tick_to(int tick) {
        while (static_cast<int>(sim.tick_count()) < tick) {
            sim.tick();
        }
    }

    std::vector<int> ticks(const char* global) {
        REQUIRE(run(std::string("listed = table.concat(") + global + ", ' ')"));
        std::istringstream in(text("listed"));
        std::vector<int> out;
        for (int t = 0; in >> t;) {
            out.push_back(t);
        }
        return out;
    }
};

} // namespace

TEST_CASE("An army hears OnIntelChange only in its recon tick, when the tick modulo the army "
          "count is its index",
          "[tick-phases][recon]") {
    const int armies = GENERATE(2, 3, 4);
    const int viewer = GENERATE_COPY(range(0, armies));
    Recon w(armies);
    w.eye("eye", viewer, 100.0f, 100.0f, "Vision", 20.0f);
    const int rival = (viewer + 1) % armies;
    REQUIRE(w.run("made = {} seen = {} B" + std::to_string(viewer) +
                  ".OnIntelChange = function(self, blip, kind, on)"
                  "   if kind == 'LOSNow' and on then table.insert(seen, GetGameTick()) end"
                  " end"
                  " ForkThread(function()"
                  "   for i = 1, " +
                  std::to_string(2 * armies) +
                  " do"
                  "     CreateUnit('tank', " +
                  std::to_string(rival + 1) +
                  ", 94 + 1.5 * i, 10, 105)"
                  "     table.insert(made, GetGameTick())"
                  "     WaitTicks(1)"
                  "   end"
                  " end)"));
    w.tick_to(4 * armies);
    std::vector<int> expected;
    for (const int made : w.ticks("made")) {
        expected.push_back(w.turn_after(made, viewer));
    }
    REQUIRE(expected.size() == static_cast<size_t>(2 * armies));
    CHECK(w.ticks("seen") == expected);
}

TEST_CASE("An army's recon tick meets the units it sees by their army, then as made",
          "[tick-phases][recon]") {
    Recon w(3);
    w.eye("eye", 0, 100.0f, 100.0f, "Vision", 20.0f);
    w.make("third", "tank", 3, 105.0f, 100.0f);
    w.make("second", "tank", 2, 95.0f, 100.0f);
    REQUIRE(w.run("met = {} B0.OnIntelChange = function(self, blip, kind, on)"
                  "   if kind == 'LOSNow' and on then"
                  "     table.insert(met, blip:GetSource() == third and 3 or 2)"
                  "   end"
                  " end"));
    w.tick_to(7);
    CHECK(w.ticks("met") == std::vector<int>{2, 3});
}

TEST_CASE("IsSeenNow and the UI read their army's last recon tick, not where the unit is now",
          "[tick-phases][recon]") {
    const int armies = GENERATE(2, 3);
    const int viewer = GENERATE_COPY(range(0, armies));
    Recon w(armies);
    w.eye("eye", viewer, 100.0f, 100.0f, "Vision", 20.0f);
    const osc::u32 tank =
        w.make("tank", "tank", (viewer + 1) % armies + 1, 105.0f, 100.0f).entity_id();
    const int away = w.turn_after(2 * armies + 1, (viewer + 1) % armies);
    const int back = away + 2 * armies + 1;
    const int last = back + 2 * armies;
    REQUIRE(w.run("at = {} reads = {} ForkThread(function()"
                  "   local blip = tank:GetBlip(" +
                  std::to_string(viewer + 1) +
                  ")"
                  "   while GetGameTick() < " +
                  std::to_string(last) +
                  " do"
                  "     local t = GetGameTick()"
                  "     if t == " +
                  std::to_string(away) +
                  " then Warp(tank, {200, 10, 200}) end"
                  "     if t == " +
                  std::to_string(back) +
                  " then Warp(tank, {105, 10, 100}) end"
                  "     if blip == nil then blip = tank:GetBlip(" +
                  std::to_string(viewer + 1) +
                  ") end"
                  "     table.insert(at, t)"
                  "     table.insert(reads, (blip and blip:IsSeenNow(" +
                  std::to_string(viewer + 1) +
                  ")) and 1 or 0)"
                  "     WaitTicks(1)"
                  "   end"
                  " end)"));
    const auto in_sight_after = [&](int t) {
        const int recon = t - ((t - viewer) % armies + armies) % armies;
        return recon >= 1 && (recon < away || recon >= back);
    };
    std::vector<int> drawn;
    std::vector<int> drawn_expected;
    while (static_cast<int>(w.sim.tick_count()) <= last) {
        w.sim.tick();
        osc::sim::WorldSnapshot snap;
        osc::sim::capture_world(w.sim, snap);
        const osc::sim::EntityRecord* r = snap.find(tank);
        drawn.push_back(r && ((r->los_now >> viewer) & 1u) != 0 ? 1 : 0);
        drawn_expected.push_back(in_sight_after(static_cast<int>(w.sim.tick_count())) ? 1 : 0);
    }
    std::vector<int> expected;
    for (const int t : w.ticks("at")) {
        expected.push_back(in_sight_after(t - 1) ? 1 : 0);
    }
    CHECK(w.ticks("reads") == expected);
    CHECK(drawn == drawn_expected);
}

TEST_CASE("A jammer's fakes are made, and known fake, only in the recon tick of the army that "
          "holds them, the cycle after its blip",
          "[tick-phases][recon]") {
    const int armies = GENERATE(2, 3);
    const int viewer = GENERATE_COPY(range(0, armies));
    Recon w(armies);
    Unit& radar = w.eye("radar", viewer, 100.0f, 100.0f, "Radar", 80.0f);
    const int rival = (viewer + 1) % armies;
    const int made = w.turn_after(2 * armies + 1, rival);
    w.tick_to(made - 1);
    Unit& jammer = w.make("jammer", "tank", rival + 1, 120.0f, 100.0f);
    jammer.set_jammer(2, 5.0f, 10.0f);
    jammer.add_intel("Jammer", 0.0f);
    jammer.enable_intel("Jammer");
    const osc::u32 id = jammer.entity_id();
    int blip = 0;
    int fakes = 0;
    while (fakes == 0 && static_cast<int>(w.sim.tick_count()) < made + 3 * armies) {
        w.sim.tick();
        const auto* recon = w.sim.entity_recon(id);
        if (blip == 0 && recon && (*recon)[static_cast<size_t>(viewer)].radar) {
            blip = static_cast<int>(w.sim.tick_count());
        }
        if (!w.sim.held_fakes(viewer).empty()) {
            fakes = static_cast<int>(w.sim.tick_count());
        }
    }
    const int first = w.turn_after(made, viewer);
    CHECK(blip == first);
    CHECK(fakes == first + armies);

    const int omni = w.turn_after(fakes + 1, rival);
    w.tick_to(omni - 1);
    radar.add_intel("Omni", 80.0f);
    radar.enable_intel("Omni");
    int known = 0;
    int hidden = 0;
    while ((known == 0 || hidden == 0) &&
           static_cast<int>(w.sim.tick_count()) < omni + 2 * armies) {
        w.sim.tick();
        const auto held = w.sim.held_fakes(viewer);
        if (known == 0 && !held.empty() &&
            std::all_of(held.begin(), held.end(), [](const auto& f) { return f.known_fake; })) {
            known = static_cast<int>(w.sim.tick_count());
        }
        osc::sim::WorldSnapshot snap;
        osc::sim::capture_world(w.sim, snap);
        if (hidden == 0 &&
            std::none_of(snap.fake_blips.begin(), snap.fake_blips.end(),
                         [&](const osc::sim::FakeBlipRecord& f) { return f.viewer == viewer; })) {
            hidden = static_cast<int>(w.sim.tick_count());
        }
    }
    CHECK(known == w.turn_after(omni, viewer));
    CHECK(hidden == known);
}

TEST_CASE("A unit destroyed in a tick lights its army's recon tick until it goes, and its blip "
          "goes in the next recon tick of the army that holds it",
          "[tick-phases][recon]") {
    const int armies = GENERATE(2, 3);
    const int viewer = GENERATE_COPY(range(0, armies));
    const int rival = (viewer + 1) % armies;
    Recon w(armies);
    w.eye("eye", viewer, 100.0f, 100.0f, "Vision", 20.0f);
    w.eye("x", rival, 110.0f, 100.0f, "Vision", 25.0f);
    w.make("y", "tank", viewer + 1, 88.0f, 100.0f);
    const int doomed = w.turn_after(3 * armies, rival);
    REQUIRE(w.run("lost = 0 B" + std::to_string(rival) +
                  ".OnIntelChange = function(self, blip, kind, on)"
                  "   if kind == 'LOSNow' and not on and blip:GetSource() == y then"
                  "     lost = GetGameTick()"
                  "   end"
                  " end"
                  " reads = {} ForkThread(function()"
                  "   while GetGameTick() < " +
                  std::to_string(doomed) +
                  " do WaitTicks(1) end"
                  "   xb = x:GetBlip(" +
                  std::to_string(viewer + 1) +
                  ")"
                  "   EM.Destroy(x)"
                  "   for i = 0, " +
                  std::to_string(2 * armies) +
                  " do"
                  "     table.insert(reads, xb:IsSeenNow(" +
                  std::to_string(viewer + 1) +
                  ") and 1 or 0)"
                  "     WaitTicks(1)"
                  "   end"
                  " end)"));
    w.tick_to(doomed + 3 * armies);
    const int forgotten = w.turn_after(doomed, viewer);
    std::vector<int> expected;
    for (int t = doomed; t <= doomed + 2 * armies; ++t) {
        expected.push_back(t <= forgotten ? 1 : 0);
    }
    CHECK(w.ticks("reads") == expected);
    CHECK(w.ticks("gone") == std::vector<int>{forgotten});
    CHECK(w.number("lost") == static_cast<f32>(doomed + armies));
}

namespace {

void callbacks_module(Phases& w) {
    REQUIRE(
        w.run("modules = modules or {}"
              " modules['/lua/SimCallbacks.lua'] = {DoCallback = function(name, data, units)"
              "   _G[name](data, units)"
              " end}"
              " function import(path) return modules[path] or error('no module ' .. path) end"));
}

osc::sim::SimCallbackEntry callback(const char* name) {
    osc::sim::SimCallbackEntry cb;
    cb.func_name = name;
    return cb;
}

UnitCommand order(CommandType type, osc::u32 target_id) {
    UnitCommand c;
    c.type = type;
    c.target_id = target_id;
    return c;
}

} // namespace

TEST_CASE("A SimCallback runs before its beat's tick: GetGameTick is the last tick's",
          "[tick-phases][commands]") {
    Phases w;
    callbacks_module(w);
    REQUIRE(w.run("seen = 0 forked = 0"
                  " function Note()"
                  "   seen = GetGameTick()"
                  "   ForkThread(function() forked = GetGameTick() end)"
                  " end"));
    w.sim.tick();
    w.sim.tick();
    w.sim.submit_callback(callback("Note"));
    w.sim.tick();
    CHECK(w.number("seen") == 2.0f);
    CHECK(w.number("forked") == 3.0f);
    CHECK(w.sim.tick_count() == 3);
}

TEST_CASE("A Lua thread's IssueClearCommands ends a unit's task inside the call",
          "[tick-phases][commands]") {
    const std::string task = GENERATE("reclaim", "repair", "capture", "script");
    CAPTURE(task);
    Phases w;
    Unit& eng = w.make("eng", "tank", 1, 100.0f, 100.0f);
    eng.set_build_rate(10.0f);
    eng.set_max_build_distance(10.0f);
    REQUIRE(
        w.run("stopped = 0"
              " function Plain:OnStopReclaim() stopped = GetGameTick() end"
              " function Plain:OnStopBuild() stopped = GetGameTick() end"
              " function Plain:OnFailedCapture() stopped = GetGameTick() end"
              " function Plain:GetReclaimCosts() return 100, 0, 30 end"
              " Wait = {TaskTick = function() return 1 end,"
              "   OnDestroy = function() stopped = GetGameTick() end}"
              " Wait.__index = Wait"
              " modules = {['/lua/sim/tasks/Wait.lua'] = {Wait = Wait}}"
              " function import(path) return modules[path] or error('no module ' .. path) end"));
    if (task == "reclaim") {
        eng.add_category("RECLAIM");
        auto wreck = std::make_unique<osc::sim::Prop>();
        wreck->set_position({102.0f, 10.0f, 100.0f});
        wreck->reclaim_mass_max = 30.0f;
        wreck->reclaimable_category = true;
        const osc::u32 id = w.sim.entity_registry().register_entity(std::move(wreck));
        eng.push_command(order(CommandType::Reclaim, id), false);
    } else if (task == "repair" || task == "capture") {
        Unit& target = w.make("target", "t1", task == "repair" ? 1 : 2, 102.0f, 100.0f);
        target.set_health(5.0f);
        eng.push_command(order(task == "repair" ? CommandType::Repair : CommandType::Capture,
                               target.entity_id()),
                         false);
    } else {
        lua_State* L = w.state.raw();
        REQUIRE(w.run("args = {TaskName = 'Wait'}"));
        lua_getglobal(L, "args");
        UnitCommand script;
        script.type = CommandType::Script;
        const auto bytes = osc::sim::lua_to_bytes(L, -1);
        lua_pop(L, 1);
        REQUIRE(bytes);
        script.script_args = *bytes;
        eng.push_command(script, false);
    }
    const auto working = [&] {
        return eng.is_reclaiming() || eng.is_repairing() || eng.is_capturing() ||
               eng.has_script_task();
    };
    for (int t = 0; t < 5 && !working(); ++t) {
        w.sim.tick();
    }
    REQUIRE(working());
    REQUIRE(w.number("stopped") == 0.0f);
    REQUIRE(w.run("cleared = 0 after = 0 ForkThread(function()"
                  "   IssueClearCommands({eng})"
                  "   cleared = GetGameTick()"
                  "   after = stopped"
                  " end)"));
    w.sim.tick();
    CHECK(w.number("cleared") > 0.0f);
    CHECK(w.number("after") == w.number("cleared"));
    CHECK_FALSE(working());
}

namespace {

void command(Phases& w, const char* name) {
    w.sim.set_pause_holds(true);
    w.sim.schedule_callback(0, callback(name));
}

void player_stop(Phases& w, const Unit& unit) {
    UnitCommand stop;
    stop.type = CommandType::Stop;
    w.sim.set_human_input_active(true);
    w.sim.route_command({unit.entity_id()}, stop, true);
    w.sim.set_human_input_active(false);
}

} // namespace

TEST_CASE("A pause requested by a command of a beat stops that beat's tick",
          "[tick-phases][beats]") {
    Phases w;
    REQUIRE(w.run("ran = {} ForkThread(function()"
                  "   while true do table.insert(ran, GetGameTick()) WaitTicks(1) end"
                  " end)"));
    w.sim.tick();
    w.sim.tick();
    command(w, osc::sim::kRequestPauseCallback);
    for (int b = 0; b < 3; ++b) {
        w.sim.tick();
    }
    CHECK(w.sim.paused());
    CHECK(w.sim.tick_count() == 2);
    CHECK(w.sim.beat_count() == 5);
    CHECK(w.run("assert(table.getn(ran) == 2 and ran[2] == 2)"));
}

TEST_CASE("A Stop given to a building engineer while paused ends its build before the game resumes",
          "[tick-phases][beats]") {
    Phases w;
    Unit& eng = w.make("eng", "tank", 1, 100.0f, 100.0f);
    eng.add_category("ENGINEER");
    eng.set_build_rate(10.0f);
    eng.set_max_build_distance(10.0f);
    REQUIRE(w.run("started = 0 stopped = 0"
                  " function Plain:OnStartBuild() started = GetGameTick() end"
                  " function Plain:OnStopBuild() stopped = GetGameTick() end"));
    UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.blueprint_id = "fac";
    build.target_pos = {104.0f, 10.0f, 100.0f};
    eng.push_command(build, false);
    for (int t = 0; t < 10 && w.number("started") == 0; ++t) {
        w.sim.tick();
    }
    REQUIRE(w.number("started") > 0);
    REQUIRE(eng.build_target_id() != 0);
    command(w, osc::sim::kRequestPauseCallback);
    w.sim.tick();
    REQUIRE(w.sim.paused());
    const osc::u32 held = w.sim.tick_count();
    player_stop(w, eng);
    w.sim.tick();
    CHECK(w.sim.paused());
    CHECK(w.sim.tick_count() == held);
    CHECK(w.number("stopped") == static_cast<f32>(held));
    CHECK(eng.build_target_id() == 0);
    CHECK(eng.command_queue().empty());
    command(w, osc::sim::kResumeCallback);
    w.sim.tick();
    CHECK_FALSE(w.sim.paused());
    CHECK(w.sim.tick_count() == held + 1);
}

TEST_CASE("A unit a command destroys while paused hears OnDestroy in that beat",
          "[tick-phases][beats]") {
    Phases w;
    callbacks_module(w);
    const osc::u32 id = w.make("victim", "tank", 1, 100.0f, 100.0f).entity_id();
    REQUIRE(w.run("gone = -1 function Plain:OnDestroy() gone = GetGameTick() end"
                  " function Doom() EM.Destroy(victim) end"));
    w.sim.tick();
    command(w, osc::sim::kRequestPauseCallback);
    w.sim.tick();
    REQUIRE(w.sim.paused());
    const osc::u32 held = w.sim.tick_count();
    w.sim.submit_callback(callback("Doom"));
    w.sim.tick();
    CHECK(w.sim.paused());
    CHECK(w.sim.tick_count() == held);
    CHECK(w.number("gone") == static_cast<f32>(held));
    CHECK(w.sim.entity_registry().find(id) == nullptr);
}

TEST_CASE("A single step runs the tick of its beat, and the game stays paused",
          "[tick-phases][beats]") {
    Phases w;
    w.sim.tick();
    command(w, osc::sim::kRequestPauseCallback);
    w.sim.tick();
    REQUIRE(w.sim.paused());
    const osc::u32 held = w.sim.tick_count();
    command(w, osc::sim::kSingleStepCallback);
    w.sim.tick();
    CHECK(w.sim.tick_count() == held + 1);
    w.sim.tick();
    w.sim.tick();
    CHECK(w.sim.paused());
    CHECK(w.sim.tick_count() == held + 1);
}

TEST_CASE("After the game ends its beats apply commands and destroy without a tick",
          "[tick-phases][beats]") {
    Phases w;
    callbacks_module(w);
    w.make("victim", "tank", 1, 100.0f, 100.0f);
    REQUIRE(w.run("gone = -1 called = -1 function Plain:OnDestroy() gone = GetGameTick() end"
                  " function Doom() called = GetGameTick() EM.Destroy(victim) end"));
    w.sim.tick();
    w.sim.set_game_ended(true);
    w.sim.tick();
    REQUIRE(w.sim.game_over());
    const osc::u32 over = w.sim.tick_count();
    w.sim.submit_callback(callback("Doom"));
    w.sim.tick();
    w.sim.tick();
    CHECK(w.sim.tick_count() == over);
    CHECK(w.number("called") == static_cast<f32>(over));
    CHECK(w.number("gone") == static_cast<f32>(over));
}

TEST_CASE("Paused on a collection tick, each beat collects the Lua state", "[tick-phases][beats]") {
    Phases w;
    callbacks_module(w);
    REQUIRE(w.run("weak = setmetatable({}, {__mode = 'v'})"
                  " function Litter() weak[1] = {} end"));
    while (w.sim.tick_count() < 70) {
        w.sim.tick();
    }
    command(w, osc::sim::kRequestPauseCallback);
    w.sim.tick();
    REQUIRE(w.sim.paused());
    REQUIRE(w.sim.tick_count() == 70);
    w.sim.submit_callback(callback("Litter"));
    w.sim.tick();
    CHECK(w.run("assert(weak[1] == nil)"));
}

TEST_CASE("A source that leaves takes its pause with it", "[tick-phases][beats]") {
    Phases w;
    w.sim.begin_command_sources();
    w.sim.set_command_source(0, 0, true);
    w.sim.set_command_source(1, 1, true);
    w.sim.set_pause_holds(true);
    w.sim.request_pause(1);
    REQUIRE(w.sim.paused());
    w.sim.terminate_command_source(1);
    CHECK_FALSE(w.sim.paused());
}

TEST_CASE("The sim's beat is profiled as Sim::tick", "[tick-phases][beats]") {
    Phases w;
    auto& profiler = osc::Profiler::instance();
    profiler.set_enabled(true);
    profiler.begin_frame();
    w.sim.tick();
    const double us = profiler.frame_zone_us("Sim::tick");
    profiler.end_frame();
    profiler.set_enabled(false);
    CHECK(us > 0.0);
}

TEST_CASE("Two beats in a frame give the user two Sync tables", "[tick-phases][beats]") {
    Phases w;
    osc::lua::LuaState ui;
    REQUIRE(w.run("function ResetSyncTable() Sync = {} end ResetSyncTable()"
                  " ForkThread(function()"
                  "   while true do Sync.Tick = GetGameTick() WaitTicks(1) end"
                  " end)"));
    REQUIRE(ui.do_string("seen = ''"
                         " function OnSync() seen = seen .. tostring(Sync.Tick) .. ',' end")
                .ok());
    w.sim.set_beat_observer(
        [&](const osc::sim::SimState& s) { osc::lua::queue_sync(s.lua_state(), ui.raw()); });
    w.sim.tick();
    w.sim.tick();
    for (int b = 0; b < 2; ++b) {
        osc::lua::sync_beat(w.state.raw(), ui.raw());
    }
    lua_State* U = ui.raw();
    lua_getglobal(U, "seen");
    CHECK(std::string(lua_tostring(U, -1)) == "1,2,");
    lua_pop(U, 1);
}
