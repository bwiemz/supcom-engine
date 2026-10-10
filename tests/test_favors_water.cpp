#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/game_setup.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::f32;
using osc::blueprints::NamedFootprint;
using osc::blueprints::UnitFootprints;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;
namespace oc = osc::blueprints::occupancy;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr f32 kWater = 5.0f;

bool dry(f32 x, f32 z) {
    return x >= 60.0f && x <= 68.0f && z < 96.0f;
}

// A sea 2 deep, but for a low strip of land from x = 60 to 68, with gentle
// shores, that ends at z = 96.
std::unique_ptr<osc::map::Terrain> sea_terrain() {
    std::vector<osc::u16> heights(129 * 129, 384);
    for (size_t z = 0; z < 96; ++z) {
        for (size_t x = 56; x <= 72; ++x) {
            const int from_shore =
                std::min<int>(static_cast<int>(x) - 56, 72 - static_cast<int>(x));
            heights[z * 129 + x] = static_cast<osc::u16>(384 + std::min(from_shore, 4) * 80);
        }
    }
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    return std::make_unique<osc::map::Terrain>(std::move(hm), kWater, true);
}

void sea(SimState& sim) {
    sim.set_terrain(sea_terrain());
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

Unit* salem(SimState& sim, f32 x, f32 z, bool favors_water = true) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(20.0f);
    u->set_motion_type("RULEUMT_AmphibiousFloating");
    u->set_position({x, kWater, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    drive.max_brake = 50.0f;
    u->set_drive(drive);
    UnitFootprints f;
    f.main.size_x = 5;
    f.main.size_z = 5;
    f.main.caps = oc::kLand | oc::kWater;
    f.alt.size_x = 4;
    f.alt.size_z = 4;
    f.alt.caps = oc::kWater;
    u->set_footprints(f);
    if (favors_water) {
        u->add_category("FAVORSWATER");
    }
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

UnitCommand move_to(Vector3 at, osc::u32 id = 0) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = at;
    c.command_id = id;
    return c;
}

} // namespace

TEST_CASE("A FAVORSWATER unit on water paths on its alt footprint to water, not to land",
          "[favorswater]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sea(sim);
    Unit* to_water = salem(sim, 20.0f, 20.0f);
    to_water->push_command(move_to({40.0f, 0.0f, 20.0f}), true);
    Unit* to_land = salem(sim, 20.0f, 60.0f);
    to_land->push_command(move_to({64.0f, 0.0f, 60.0f}), true);
    Unit* plain = salem(sim, 20.0f, 100.0f, false);
    plain->push_command(move_to({40.0f, 0.0f, 100.0f}), true);
    sim.tick();
    CHECK(to_water->footprint().caps == oc::kWater);
    CHECK(to_land->footprint().caps == (oc::kLand | oc::kWater));
    CHECK(plain->footprint().caps == (oc::kLand | oc::kWater));
}

TEST_CASE("A FAVORSWATER group keeps to water only if all its destinations are on water",
          "[favorswater]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sea(sim);
    Unit* a = salem(sim, 20.0f, 20.0f);
    Unit* b = salem(sim, 20.0f, 30.0f);
    a->push_command(move_to({40.0f, 0.0f, 20.0f}, 7), true);
    b->push_command(move_to({64.0f, 0.0f, 30.0f}, 7), true);
    Unit* c = salem(sim, 20.0f, 100.0f);
    Unit* d = salem(sim, 20.0f, 110.0f);
    c->push_command(move_to({40.0f, 0.0f, 100.0f}, 8), true);
    d->push_command(move_to({40.0f, 0.0f, 110.0f}, 8), true);
    sim.tick();
    CHECK(a->footprint().caps == (oc::kLand | oc::kWater));
    CHECK(b->footprint().caps == (oc::kLand | oc::kWater));
    CHECK(c->footprint().caps == oc::kWater);
    CHECK(d->footprint().caps == oc::kWater);
}

TEST_CASE("A FAVORSWATER unit sails round a strip of land it could cross", "[favorswater]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sea(sim);
    Unit* u = salem(sim, 40.0f, 40.0f);
    u->push_command(move_to({90.0f, 0.0f, 40.0f}), true);
    bool landed = false;
    for (int t = 0; t < 400; ++t) {
        sim.tick();
        landed = landed || dry(u->position().x, u->position().z);
    }
    CHECK(u->position().x > 85.0f);
    CHECK_FALSE(landed);
}

TEST_CASE("FAF's ForceAltFootprint holds a unit to its alt footprint", "[favorswater]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sea(sim);
    Unit* u = salem(sim, 20.0f, 60.0f, false);
    u->set_force_alt_footprint(true);
    u->push_command(move_to({64.0f, 0.0f, 60.0f}), true);
    sim.tick();
    CHECK(u->footprint().caps == oc::kWater);
    u->set_force_alt_footprint(false);
    CHECK(u->footprint().caps == (oc::kLand | oc::kWater));
}

namespace {

NamedFootprint footprint_class(const char* name, int size, osc::u8 caps, f32 min_depth,
                               f32 max_depth) {
    NamedFootprint f;
    f.name = name;
    f.size_x = static_cast<osc::u8>(size);
    f.size_z = static_cast<osc::u8>(size);
    f.caps = caps;
    f.min_water_depth = min_depth;
    f.max_water_depth = max_depth;
    f.max_slope = 0.75f;
    return f;
}

// Retail's URS0201 as Moho paths it: WaterLand5x5, and Water4x4 for its
// AltMotionType.
struct MohoSea {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    SimState sim{state.raw(), &store};

    MohoSea() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        store.add_footprint_class(footprint_class("WaterLand5x5", 5, oc::kLand | oc::kWater, 0, 5));
        store.add_footprint_class(footprint_class("Water4x4", 4, oc::kWater, 1.5f, 0));
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.set_terrain(sea_terrain());
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        sim.set_moho_pathing(true);
        lua_State* L = state.raw();
        REQUIRE(state
                    .do_string("return {BlueprintId = 'salem', Categories = {'NAVAL', 'MOBILE',"
                               " 'FAVORSWATER'}, Defense = {MaxHealth = 100},"
                               " Footprint = {SizeX = 2, SizeZ = 8}, SizeX = 1.5, SizeZ = 5.4,"
                               " Physics = {MotionType = 'RULEUMT_AmphibiousFloating',"
                               " AltMotionType = 'RULEUMT_Water', MaxSpeed = 20,"
                               " MaxAcceleration = 50, MaxBrake = 50, TurnRate = 720}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "salem");
        lua_getglobal(L, "Plain");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(f32 x, f32 z) {
        REQUIRE(state
                    .do_string("made = CreateUnit('salem', 1, " + std::to_string(x) + ", 5, " +
                               std::to_string(z) + ")")
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

} // namespace

TEST_CASE("With Moho pathing a FAVORSWATER unit sails round a strip of land it could cross",
          "[favorswater]") {
    MohoSea w;
    Unit* u = w.make(40.0f, 40.0f);
    u->push_command(move_to({90.0f, 0.0f, 40.0f}), true);
    bool landed = false;
    bool moho = false;
    for (int t = 0; t < 600 && !u->command_queue().empty(); ++t) {
        w.sim.tick();
        landed = landed || dry(u->position().x, u->position().z);
        moho = moho || u->navigator().moho_active();
    }
    CHECK(moho);
    CHECK(u->position().x > 85.0f);
    CHECK_FALSE(landed);
}

TEST_CASE("FAF's Unit:ForceAltFootprint sets and clears the forced alt footprint",
          "[favorswater]") {
    MohoSea w;
    Unit* u = w.make(40.0f, 40.0f);
    REQUIRE(u->footprint().caps == (oc::kLand | oc::kWater));
    REQUIRE(w.state.do_string("made:ForceAltFootprint(true)").ok());
    CHECK(u->footprint().caps == oc::kWater);
    CHECK(u->footprint().min_water_depth == 1.5f);
    REQUIRE(w.state.do_string("made:ForceAltFootprint(false)").ok());
    CHECK(u->footprint().caps == (oc::kLand | oc::kWater));
}
