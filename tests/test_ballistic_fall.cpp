// A killed aircraft's fall, as Moho's CUnitMotion drives it (faf-re unit/CUnitMotion.cpp,
// CalcMoveAir's dead branch and CalcMoveBallistic): UMS_Ballistic, a tumble drawn from the
// sim's stream, gravity's 4.9 a second squared along its last move, and Crashed where the step
// meets the ground or the water, its script told OnImpact there.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/flight_math.hpp"
#include "sim/sim_random.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

/// A sim on flat ground at height 10, under water to 15 if `water`, with two aircraft
/// blueprints whose script class records what it is told.
struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit World(bool water = false) {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(257) * 257, 1280);
        osc::map::Heightmap hm(256, 256, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(
            std::make_unique<osc::map::Terrain>(std::move(hm), water ? 15.0f : 0.0f, water));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'gunship', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 2, SizeY = 1, SizeZ = 2,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Air = {CanFly = true, MaxAirspeed = 12, KMove = 0.8, KMoveDamping = 2,"
                 " StartTurnDistance = 5, AutoLandTime = 0},"
                 " Physics = {MotionType = 'RULEUMT_Air', MaxSpeed = 12, Elevation = 10}}",
                 "{BlueprintId = 'heavy', Categories = {'AIR', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 12, SizeY = 4, SizeZ = 12,"
                 " Footprint = {SizeX = 12, SizeZ = 12},"
                 " Air = {CanFly = true, MaxAirspeed = 5, StartTurnDistance = 5,"
                 " AutoLandTime = 0},"
                 " Physics = {MotionType = 'RULEUMT_Air', MaxSpeed = 5, Elevation = 10}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string(R"(
                        told = {}
                        Flier = setmetatable({}, {__index = moho.unit_methods})
                        Flier.__index = Flier
                        function Flier:OnKilled() end
                        function Flier:OnLayerChange(new, old)
                            table.insert(told, 'layer ' .. new .. ' ' .. old)
                        end
                        function Flier:OnMotionStateChange(new, old)
                            table.insert(told, 'state ' .. new .. ' ' .. old)
                        end
                        function Flier:OnImpact(with)
                            table.insert(told, 'impact ' .. with)
                        end
                    )")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"gunship", "heavy"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Flier");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit& make(const char* bp, f32 x, f32 z) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', 1, " +
                               std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && !u.is_dying()) found = &u;
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return *found;
    }

    /// Run `code`, answering its error (empty if none).
    std::string run(const std::string& code) {
        auto r = state.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }

    /// What the scripts were told, in order, joined by ", ".
    std::string told() {
        REQUIRE(state.do_string("told_text = table.concat(told, ', ')").ok());
        lua_getglobal(state.raw(), "told_text");
        std::string out = lua_tostring(state.raw(), -1);
        lua_pop(state.raw(), 1);
        return out;
    }
};

/// Fly `u` up to its height first: made on the ground, an aircraft rises on its own.
void lift_off(World& w, Unit& u) {
    for (int i = 0; i < 60 && u.position().y < 18.0f; ++i) w.sim.tick();
    REQUIRE(u.layer() == "Air");
    REQUIRE(u.position().y > 15.0f);
}

} // namespace

TEST_CASE("The tumble's draws are within each axis' inverse inertia, clamped to [0.25, 4]",
          "[ballistic]") {
    const Vector3 unit_box = osc::sim::box_inertia(1.0f, 1.0f, 1.0f);
    // 1 / (mass x inertia): a light body's is past 4, a heavy one's under 0.25
    const Vector3 light = osc::sim::tumble_range(0.01f, unit_box);
    CHECK(light.x == 4.0f);
    CHECK(light.z == 4.0f);
    const Vector3 heavy = osc::sim::tumble_range(1000.0f, unit_box);
    CHECK(heavy.y == 0.25f);
    const Vector3 between = osc::sim::tumble_range(3.0f, unit_box);
    CHECK(between.x == Approx(1.0f / (3.0f * unit_box.x)));
    // A NaN passes the cap and is floored
    const Vector3 none = osc::sim::tumble_range(std::nanf(""), unit_box);
    CHECK(none.x == 0.25f);

    // The torque per unit mass: each draw times its axis' inertia, turned into the world
    const Vector3 inertia{2.0f, 3.0f, 4.0f};
    const Vector3 level = osc::sim::tumble_torque({0, 0, 0, 1}, 5.0f, inertia, {1.0f, -1.0f, 0.5f});
    CHECK(level.x == Approx(2.0f));
    CHECK(level.y == Approx(-3.0f));
    CHECK(level.z == Approx(2.0f));
    // A quarter turn about Y takes the body's x axis to the world's -z
    const f32 s = std::sqrt(0.5f);
    const Vector3 turned = osc::sim::tumble_torque({0, s, 0, s}, 5.0f, inertia, {1.0f, 0.0f, 0.0f});
    CHECK(turned.x == Approx(0.0f).margin(1e-6));
    CHECK(turned.z == Approx(-2.0f));
    // A massless body has none
    const Vector3 massless = osc::sim::tumble_torque({0, 0, 0, 1}, 0.0f, inertia, {1, 1, 1});
    CHECK(massless.x == 0.0f);

    // The torque is kept up: the momentum gains it every step
    const osc::sim::SpinStep step =
        osc::sim::air_torque_step({0, 0, 0, 1}, {0.5f, 0, 0}, inertia, {1.0f, 0, 0}, 0.1f);
    CHECK(step.momentum.x == Approx(0.6f));
}

TEST_CASE("Moho's SurfaceIntersection: the ground, the water over it, a start under it",
          "[ballistic]") {
    std::vector<osc::u16> heights(static_cast<size_t>(17) * 17, 1280);
    const osc::map::Terrain dry(osc::map::Heightmap(16, 16, 1.0f / 128.0f, heights), 0.0f, false);
    const osc::map::Terrain wet(osc::map::Heightmap(16, 16, 1.0f / 128.0f, heights), 15.0f, true);
    // Straight down from 20 by 12: the ground at 10 is 10 along
    const auto ground = dry.surface_intersect(8, 20, 8, 0, -1, 0, 12);
    REQUIRE(ground);
    CHECK(*ground == Approx(10.0f));
    // Too short to reach it
    CHECK_FALSE(dry.surface_intersect(8, 20, 8, 0, -1, 0, 5));
    // The water at 15 is met first
    const auto water = wet.surface_intersect(8, 20, 8, 0, -1, 0, 12);
    REQUIRE(water);
    CHECK(*water == Approx(5.0f));
    // A start under the water meets it at once
    const auto under = wet.surface_intersect(8, 12, 8, 0, -1, 0, 1);
    REQUIRE(under);
    CHECK(*under == 0.0f);
}

TEST_CASE("A killed aircraft in flight falls by Moho's gravity and is Crashed on the ground",
          "[ballistic]") {
    World w;
    Unit& u = w.make("gunship", 128.0f, 128.0f);
    lift_off(w, u);
    REQUIRE(w.run("told = {} made:Kill()").empty());
    CHECK(u.is_dying());
    CHECK(u.motion_state() == Unit::MotionState::None);

    // The next tick it starts to fall: Ballistic, its tumble drawn
    w.sim.tick();
    CHECK(u.motion_state() == Unit::MotionState::Ballistic);
    CHECK(u.is_crashing());
    CHECK(u.layer() == "Air");
    CHECK(w.told() == "state Ballistic None");
    const Vector3 torque = u.tumble_torque();
    CHECK(torque.x * torque.x + torque.y * torque.y + torque.z * torque.z > 0.0f);

    // Each tick it falls along its last move, and gravity adds 4.9 x 0.01 to that
    std::vector<f32> heights{u.position().y};
    for (int i = 0; i < 200 && u.motion_state() == Unit::MotionState::Ballistic; ++i) {
        w.sim.tick();
        heights.push_back(u.position().y);
    }
    REQUIRE(u.motion_state() == Unit::MotionState::Crashed);
    REQUIRE(heights.size() >= 5);
    const f32 first = heights[2] - heights[1];
    const f32 second = heights[3] - heights[2];
    CHECK(second - first == Approx(-0.049f).margin(1e-4));

    // On the ground: Land, then OnImpact, then Crashed
    CHECK(u.layer() == "Land");
    CHECK(u.position().y == Approx(10.0f).margin(1e-3));
    CHECK(w.told() ==
          "state Ballistic None, layer Land Air, impact Terrain, state Crashed Ballistic");

    // Crashed, it moves no more
    const Vector3 at = u.position();
    for (int i = 0; i < 5; ++i) w.sim.tick();
    CHECK(u.position().x == at.x);
    CHECK(u.position().y == at.y);
    CHECK(u.position().z == at.z);
    CHECK(w.told() ==
          "state Ballistic None, layer Land Air, impact Terrain, state Crashed Ballistic");
}

TEST_CASE("A killed aircraft falling over the sea is Crashed on the water's surface",
          "[ballistic]") {
    World w(true);
    Unit& u = w.make("gunship", 128.0f, 128.0f);
    for (int i = 0; i < 80 && u.position().y < 23.0f; ++i) w.sim.tick();
    REQUIRE(u.layer() == "Air");
    REQUIRE(w.run("told = {} made:Kill()").empty());
    for (int i = 0; i < 200 && u.motion_state() != Unit::MotionState::Crashed; ++i) w.sim.tick();
    REQUIRE(u.motion_state() == Unit::MotionState::Crashed);
    CHECK(u.layer() == "Water");
    CHECK(u.position().y == Approx(15.0f).margin(1e-3));
    CHECK(w.told() ==
          "state Ballistic None, layer Water Air, impact Water, state Crashed Ballistic");
}

TEST_CASE("A heavy aircraft's tumble turns it slower than a light one's", "[ballistic]") {
    World w;
    Unit& light = w.make("gunship", 100.0f, 128.0f);
    REQUIRE(w.run("light = made").empty());
    Unit& heavy = w.make("heavy", 160.0f, 128.0f);
    REQUIRE(w.run("heavy = made").empty());
    for (int i = 0; i < 60 && (light.position().y < 18.0f || heavy.position().y < 18.0f); ++i)
        w.sim.tick();
    REQUIRE(w.run("light:Kill() heavy:Kill()").empty());
    w.sim.tick();
    REQUIRE(light.is_crashing());
    REQUIRE(heavy.is_crashing());
    // Its torque per unit mass is its draw (within its clamped inverse inertia) times its
    // inertia: the heavy one's draws are within 0.25, the light one's within 4.
    const Vector3 light_box = osc::sim::box_inertia(2.0f, 1.0f, 2.0f);
    const Vector3 heavy_box = osc::sim::box_inertia(12.0f, 4.0f, 12.0f);
    const Vector3 light_range = osc::sim::tumble_range(light.load_metric(), light_box);
    const Vector3 heavy_range = osc::sim::tumble_range(heavy.load_metric(), heavy_box);
    CHECK(heavy_range.x == 0.25f);
    CHECK(light_range.x > heavy_range.x);
    const Vector3 lt = osc::sim::to_body(light.pending_orientation(), light.tumble_torque());
    const Vector3 ht = osc::sim::to_body(heavy.pending_orientation(), heavy.tumble_torque());
    CHECK(std::abs(ht.y) <= heavy_box.y * heavy_range.y + 1e-3f);
    CHECK(std::abs(lt.y) <= light_box.y * light_range.y + 1e-3f);
}

TEST_CASE("A stunned aircraft killed in flight hangs until its stun runs out", "[ballistic]") {
    World w;
    Unit& u = w.make("gunship", 128.0f, 128.0f);
    lift_off(w, u);
    REQUIRE(w.run("made:SetStunned(0.5) made:Kill()").empty());
    const Vector3 at = u.position();
    for (int i = 0; i < 4; ++i) {
        w.sim.tick();
        INFO(i);
        CHECK(u.motion_state() == Unit::MotionState::None);
        CHECK(u.position().y == at.y);
    }
    for (int i = 0; i < 3; ++i) w.sim.tick();
    CHECK(u.motion_state() == Unit::MotionState::Ballistic);
}

TEST_CASE("A landed aircraft killed doesn't fall", "[ballistic]") {
    World w;
    Unit& u = w.make("gunship", 128.0f, 128.0f);
    u.set_layer("Land");
    REQUIRE(w.run("made:Kill()").empty());
    osc::sim::SimRandom rng(5);
    for (int i = 0; i < 3; ++i) u.step_dead_motion(w.sim.terrain(), rng, w.state.raw(), &w.sim);
    CHECK(u.motion_state() == Unit::MotionState::None);
    CHECK(u.layer() == "Land");
}

TEST_CASE("An aircraft killed while it is being built doesn't fall", "[ballistic]") {
    World w;
    Unit& u = w.make("gunship", 128.0f, 128.0f);
    lift_off(w, u);
    u.set_is_being_built(true);
    REQUIRE(w.run("told = {} made:Kill()").empty());
    const Vector3 at = u.position();
    for (int i = 0; i < 5; ++i) w.sim.tick();
    CHECK(u.motion_state() == Unit::MotionState::None);
    CHECK(u.position().y == at.y);
    CHECK(w.told().empty());
}
