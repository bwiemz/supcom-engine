// A sub leans into its dive as Moho's CUnitMotion::CalcRollHack tilts it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/game_setup.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;
namespace oc = osc::blueprints::occupancy;

namespace {

constexpr f32 kHeading = 1.2f;

osc::blueprints::NamedFootprint footprint_class(const char* name, osc::u8 caps, f32 min_depth) {
    osc::blueprints::NamedFootprint f;
    f.name = name;
    f.size_x = 2;
    f.size_z = 2;
    f.caps = caps;
    f.min_water_depth = min_depth;
    f.max_slope = 0.75f;
    return f;
}

/// A sea 5 deep; the sub dives to 1.5, as UEF's T1 sub.
struct Sea {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    explicit Sea(bool moho) {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        store.add_footprint_class(footprint_class("SurfacingSub2x2", oc::kSub | oc::kWater, 1.5f));
        store.add_footprint_class(footprint_class("Water2x2", oc::kWater, 1.5f));
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> bed(129 * 129, 0);
        sim.set_terrain(std::make_unique<osc::map::Terrain>(
            osc::map::Heightmap(128, 128, 1.0f / 128.0f, std::move(bed)), 5.0f, true));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        sim.set_moho_pathing(moho);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'sub', Categories = {'NAVAL', 'MOBILE', 'SUBMERSIBLE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 2, SizeZ = 2},"
                 " SizeX = 0.8, SizeZ = 3, Physics = {MotionType = 'RULEUMT_SurfacingSub',"
                 " Elevation = -1.5, MaxSpeed = 6, MaxAcceleration = 6, MaxBrake = 6,"
                 " TurnRate = 90}}",
                 "{BlueprintId = 'ship', Categories = {'NAVAL', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, Footprint = {SizeX = 2, SizeZ = 2},"
                 " SizeX = 0.8, SizeZ = 3, Physics = {MotionType = 'RULEUMT_Water', MaxSpeed = 6,"
                 " MaxAcceleration = 6, MaxBrake = 6, TurnRate = 90}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"sub", "ship"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(const char* bp, f32 x, f32 z) {
        REQUIRE(state
                    .do_string(std::string("CreateUnitHPR('") + bp + "', 'ARMY_1', " +
                               std::to_string(x) + ", 5, " + std::to_string(z) + ", 0, " +
                               std::to_string(kHeading) + ", 0)")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            if (e.position().x == x && e.position().z == z) {
                found = static_cast<Unit*>(&e);
            }
        });
        REQUIRE(found);
        return found;
    }
};

void order(Unit* u, CommandType type, Vector3 at = {}) {
    UnitCommand c;
    c.type = type;
    c.target_pos = at;
    u->push_command(c, type == CommandType::Dive);
}

f32 pitch_deg(const Unit& u) {
    return std::asin(osc::sim::quat_rotate(u.orientation(), {0, 0, 1}).y) * 57.29578f;
}

f32 course(const Unit& u) {
    const Vector3 nose = osc::sim::quat_rotate(u.orientation(), {0, 0, 1});
    return std::atan2(nose.x, nose.z);
}

f32 roll_deg(const Unit& u) {
    return std::asin(osc::sim::quat_rotate(u.orientation(), {1, 0, 0}).y) * 57.29578f;
}

struct Swing {
    f32 low = 0;
    f32 high = 0;
    f32 turn = 0;
    f32 roll = 0;
};

Swing watch(Sea& w, const Unit& u, int ticks) {
    Swing s;
    for (int t = 0; t < ticks; ++t) {
        w.sim.tick();
        s.low = std::min(s.low, pitch_deg(u));
        s.high = std::max(s.high, pitch_deg(u));
        s.turn = std::max(s.turn, std::abs(course(u) - kHeading));
        s.roll = std::max(s.roll, std::abs(roll_deg(u)));
    }
    return s;
}

} // namespace

TEST_CASE("A sub tips its nose up as it surfaces and down as it dives, then levels off", "[dive]") {
    const bool moho = GENERATE(false, true);
    CAPTURE(moho);
    Sea w(moho);
    Unit* u = w.make("sub", 64.0f, 64.0f);
    REQUIRE(u->layer() == "Sub");

    order(u, CommandType::Dive);
    const Swing up = watch(w, *u, 60);
    CHECK(u->layer() == "Water");
    CHECK(std::abs(up.high - 18.531f) < 0.01f);
    CHECK(up.low > -0.01f);
    CHECK(up.turn < 1e-4f);
    CHECK(up.roll < 1e-3f);
    CHECK(std::abs(pitch_deg(*u)) < 0.1f);

    order(u, CommandType::Dive);
    const Swing down = watch(w, *u, 60);
    CHECK(u->layer() == "Sub");
    CHECK(std::abs(down.low + 18.531f) < 0.01f);
    CHECK(down.high < 0.01f);
    CHECK(down.turn < 1e-4f);
    CHECK(down.roll < 1e-3f);
    CHECK(std::abs(pitch_deg(*u)) < 0.1f);
}

TEST_CASE("A sub dives nose first under way; a ship beside it stays level", "[dive]") {
    const bool moho = GENERATE(false, true);
    CAPTURE(moho);
    Sea w(moho);
    Unit* sub = w.make("sub", 30.0f, 40.0f);
    Unit* ship = w.make("ship", 40.0f, 30.0f);
    order(sub, CommandType::Dive);
    watch(w, *sub, 60);
    REQUIRE(sub->layer() == "Water");

    const Vector3 ahead{std::sin(kHeading) * 60.0f, 0, std::cos(kHeading) * 60.0f};
    order(sub, CommandType::Dive);
    order(sub, CommandType::Move, {30.0f + ahead.x, 0, 40.0f + ahead.z});
    order(ship, CommandType::Move, {40.0f + ahead.x, 0, 30.0f + ahead.z});
    f32 deepest = 0;
    f32 ship_tilt = 0;
    for (int t = 0; t < 40; ++t) {
        w.sim.tick();
        deepest = std::min(deepest, pitch_deg(*sub));
        ship_tilt = std::max({ship_tilt, std::abs(pitch_deg(*ship)), std::abs(roll_deg(*ship))});
    }
    CHECK(sub->position().x - 30.0f > 5.0f);
    CHECK(deepest < -17.0f);
    CHECK(ship->position().x - 40.0f > 5.0f);
    CHECK(ship_tilt < 0.01f);
}

TEST_CASE("A ship's hull rocks from a recoil across it, and rights itself under way", "[dive]") {
    Sea w(false);
    Unit* ship = w.make("ship", 40.0f, 30.0f);
    const Vector3 forward{std::sin(kHeading), 0, std::cos(kHeading)};
    const Vector3 right{std::cos(kHeading), 0, -std::sin(kHeading)};
    // Unit:RecoilImpulse, as retail's ShipRock weapons push one: along the
    // heading it is dropped, across it a tenth rocks the hull.
    lua_State* L = w.state.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ship->lua_table_ref());
    lua_setglobal(L, "ship");
    REQUIRE(w.state
                .do_string("ship:RecoilImpulse(" + std::to_string(forward.x) + ", 0, " +
                           std::to_string(forward.z) + ")")
                .ok());
    CHECK(std::abs(ship->recoil_impulse().x) < 1e-5f);
    CHECK(std::abs(ship->recoil_impulse().z) < 1e-5f);
    ship->add_recoil_impulse(right);
    CHECK(std::abs(ship->recoil_impulse().x - 0.1f * right.x) < 1e-5f);
    CHECK(std::abs(ship->recoil_impulse().z - 0.1f * right.z) < 1e-5f);

    // Standing, the hull isn't snapped to the water, so it doesn't rock yet
    // (Moho's MoveTo snaps a hull only while it moves or leans).
    const Swing still = watch(w, *ship, 10);
    CHECK(still.roll < 1e-3f);
    CHECK(std::abs(ship->recoil_impulse().x - 0.1f * right.x) < 1e-5f);

    // Under way it rocks: the tilt peaks at 0.073 across (atan: 4.18 deg)
    // three ticks on, then rights itself, damped by RollDamping 0.5 and
    // pulled back by RollStability 0.2.
    order(ship, CommandType::Move, {40.0f + forward.x * 60.0f, 0, 30.0f + forward.z * 60.0f});
    const Swing rock = watch(w, *ship, 30);
    CHECK(rock.roll > 4.1f);
    CHECK(rock.roll < 4.25f);
    CHECK(rock.turn < 1e-3f);
    watch(w, *ship, 10);
    CHECK(std::abs(roll_deg(*ship)) < 0.01f);
    CHECK(std::abs(ship->body_tilt().x) < 1e-4f);

    // RecoilImpulse takes three numbers.
    CHECK_FALSE(w.state.do_string("ship:RecoilImpulse(1, 2)").ok());
    CHECK_FALSE(w.state.do_string("ship:RecoilImpulse(1, 2, 'x')").ok());
}

TEST_CASE("A sub's dive holds while it is stunned or immobile, then goes on where it stopped",
          "[dive]") {
    const bool moho = GENERATE(false, true);
    const bool stun = GENERATE(false, true);
    CAPTURE(moho, stun);
    Sea w(moho);
    Unit* held = w.make("sub", 30.0f, 40.0f);
    Unit* other = w.make("sub", 64.0f, 64.0f);
    order(held, CommandType::Dive);
    order(other, CommandType::Dive);
    std::vector<f32> held_y;
    std::vector<f32> other_y;
    const auto tick = [&] {
        w.sim.tick();
        held_y.push_back(held->position().y);
        other_y.push_back(other->position().y);
    };
    for (int t = 0; t < 5; ++t) {
        tick();
    }
    REQUIRE(held_y.back() != held_y.front());
    if (stun) {
        held->set_stunned(1.0);
    } else {
        held->set_immobile(true);
    }
    const size_t from = held_y.size() - 1;
    for (int t = 0; t < 9; ++t) {
        tick();
    }
    held->set_immobile(false);
    for (int t = 0; t < 20; ++t) {
        tick();
    }
    for (size_t t = from + 1; t <= from + 9; ++t) {
        CHECK(held_y[t] == held_y[from]);
    }
    for (size_t t = from + 10; t < held_y.size(); ++t) {
        CHECK(held_y[t] == other_y[t - 9]);
    }
}
