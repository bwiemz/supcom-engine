// A moving unit given a new order in place of its own, as Moho's UNIT_IssueCommand clears its
// queue: the tasks the clear ends stop its move or leave it going.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/prop.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::CommandType;
using osc::sim::EntityId;
using osc::sim::Unit;
using osc::sim::UnitCommand;
namespace oc = osc::blueprints::occupancy;

namespace {

constexpr u32 kMap = 128;

struct World {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    World() {
        osc::sim::GameSetup game;
        game.scenario = "/maps/test/test_scenario.lua";
        game.seed = 3;
        osc::blueprints::NamedFootprint land;
        land.name = "Vehicle1x1";
        land.size_x = 1;
        land.size_z = 1;
        land.caps = oc::kLand;
        land.max_water_depth = 0.05f;
        land.max_slope = 0.75f;
        store.add_footprint_class(land);
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(kMap + 1) * (kMap + 1), 1280);
        osc::map::Heightmap hm(kMap, kMap, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.build_spatial_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_game_setup(game);
        sim.set_moho_pathing(true);
        lua_State* L = state.raw();
        for (const char* bp : {
                 "{BlueprintId = 'walker', Categories = {'LAND', 'MOBILE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.6, SizeY = 0.5, SizeZ = 0.6,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 2, MaxAcceleration = 2,"
                 "  MaxBrake = 2, TurnRate = 180, TurnRadius = 0}}",
                 "{BlueprintId = 'builder', Categories = {'LAND', 'MOBILE', 'ENGINEER', 'RECLAIM'},"
                 " Defense = {MaxHealth = 100}, SizeX = 0.6, SizeY = 0.5, SizeZ = 0.6,"
                 " Footprint = {SizeX = 1, SizeZ = 1},"
                 " Economy = {BuildRate = 10, MaxBuildDistance = 5, BuildableCategory = {'pgen'}},"
                 " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 2, MaxAcceleration = 2,"
                 "  MaxBrake = 2, TurnRate = 180, TurnRadius = 0}}",
                 "{BlueprintId = 'pgen', Categories = {'STRUCTURE'},"
                 " Defense = {MaxHealth = 100}, SizeX = 1.6, SizeY = 1, SizeZ = 1.6,"
                 " Footprint = {SizeX = 2, SizeZ = 2},"
                 " Economy = {BuildTime = 10, BuildCostEnergy = 1, BuildCostMass = 1},"
                 " Physics = {MotionType = 'RULEUMT_None'}}",
             }) {
            REQUIRE(state.do_string(std::string("return ") + bp).ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state
                    .do_string("Plain = setmetatable({}, {__index = moho.unit_methods})"
                               " Plain.__index = Plain"
                               " function Plain:CheckBuildRestriction(bp)"
                               " return self:CanBuild(bp.BlueprintId) end"
                               " function Plain:GetReclaimCosts(target) return 1, 1, 1 end")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const char* id : {"walker", "builder", "pgen"}) {
            lua_pushstring(L, id);
            lua_getglobal(L, "Plain");
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);
    }

    Unit* make(const char* bp, f32 x, f32 z) {
        REQUIRE(state
                    .do_string("made = CreateUnit('" + std::string(bp) + "', 1, " +
                               std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            auto& u = static_cast<Unit&>(e);
            if (u.blueprint_id() == bp && u.position().x == x && u.position().z == z) {
                found = &u;
            }
        });
        REQUIRE(found);
        lua_settop(state.raw(), 0);
        return found;
    }

    osc::sim::Prop* tree(f32 x, f32 z) {
        auto p = std::make_unique<osc::sim::Prop>();
        p->set_position({x, 10.0f, z});
        p->set_footprint_size(1.0f, 1.0f);
        p->reclaimable_category = true;
        p->set_collision_shape(
            {osc::sim::CollisionShapeType::BOX, 0.0f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f});
        osc::sim::Prop* raw = p.get();
        sim.entity_registry().register_entity(std::move(p));
        lua_State* L = state.raw();
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, raw);
        lua_rawset(L, -3);
        raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        return raw;
    }

    void issue(Unit& u, CommandType type, f32 x, f32 z, bool clear, EntityId target = {}) {
        UnitCommand cmd;
        cmd.type = type;
        cmd.target_pos = {x, 10.0f, z};
        cmd.target_id = target;
        if (type == CommandType::BuildMobile) {
            cmd.blueprint_id = "pgen";
        }
        sim.route_command({u.entity_id()}, cmd, clear);
    }

    f32 step(const Unit& u) {
        const osc::sim::Vector3 was = u.position();
        sim.tick();
        const f32 dx = u.position().x - was.x;
        const f32 dz = u.position().z - was.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    f32 cruise(const Unit& u) {
        f32 last = 0.0f;
        for (int t = 0; t < 40; ++t) {
            last = step(u);
        }
        return last;
    }
};

std::vector<f32> steps(World& w, const Unit& u, int n) {
    std::vector<f32> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.push_back(w.step(u));
    }
    return out;
}

std::string shown(const std::vector<f32>& s) {
    std::string out;
    for (const f32 v : s) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s%.4f", out.empty() ? "" : " ", static_cast<double>(v));
        out += buf;
    }
    return out;
}

} // namespace

TEST_CASE("A moving unit given a new order brakes only when the order, or the second order of "
          "the queue it had, is one Moho's move task stops for",
          "[order-switch]") {
    struct Case {
        const char* what;
        CommandType next;
        std::optional<CommandType> second;
        bool brakes;
    };
    const Case c =
        GENERATE(Case{"a move", CommandType::Move, std::nullopt, false},
                 Case{"a move after a move", CommandType::Move, CommandType::Move, false},
                 Case{"a move, a build behind the old one", CommandType::Move,
                      CommandType::BuildMobile, true},
                 Case{"a build", CommandType::BuildMobile, std::nullopt, true},
                 Case{"a build after a move", CommandType::BuildMobile, CommandType::Move, true});
    CAPTURE(c.what);
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    w.issue(b, CommandType::Move, 110.5f, 60.5f, true);
    if (c.second) {
        w.issue(b, *c.second, 116.0f, 60.0f, false);
    }
    const f32 top = w.cruise(b);
    REQUIRE(top > 0.19f);
    w.issue(b, c.next, 100.0f, 60.0f, true);
    const std::vector<f32> s = steps(w, b, 3);
    INFO("cruise " << top << ", then " << shown(s));
    if (c.brakes) {
        CHECK(std::abs(s[0] - (2.0f * top - 0.06f)) < 1e-3f);
        CHECK(std::abs(s[1] - (top - 0.06f)) < 1e-3f);
    } else {
        CHECK(std::abs(s[0] - top) < 1e-3f);
        CHECK(std::abs(s[1] - top) < 1e-3f);
    }
}

TEST_CASE("A moving builder given a build in reach brakes along its spline to the end, once",
          "[order-switch]") {
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    w.issue(b, CommandType::Move, 110.5f, 60.5f, true);
    const f32 top = w.cruise(b);
    REQUIRE(top > 0.19f);
    w.issue(b, CommandType::BuildMobile, b.position().x + 6.0f, 64.0f, true);
    const std::vector<f32> s = steps(w, b, 7);
    INFO("cruise " << top << ", then " << shown(s));
    CHECK(std::abs(s[0] - (2.0f * top - 0.06f)) < 1e-3f);
    CHECK(std::abs(s[1] - (top - 0.06f)) < 1e-3f);
    for (size_t i = 1; i + 1 < s.size() && s[i + 1] > 0.0f; ++i) {
        CAPTURE(i);
        CHECK(std::abs(s[i] - s[i + 1] - 0.02f) < 1e-3f);
    }
}

TEST_CASE("A moving unit given a reclaim, or a build on a site to clear, brakes from the reclaim "
          "task's start, a node a tick",
          "[order-switch]") {
    const bool build = GENERATE(false, true);
    CAPTURE(build);
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    w.issue(b, CommandType::Move, 110.5f, 60.5f, true);
    const f32 top = w.cruise(b);
    REQUIRE(top > 0.19f);
    const osc::sim::Prop* tree = w.tree(std::round(b.position().x) + 20.0f, 66.0f);
    if (build) {
        w.issue(b, CommandType::BuildMobile, tree->position().x, tree->position().z, true);
    } else {
        w.issue(b, CommandType::Reclaim, tree->position().x, tree->position().z, true,
                tree->entity_id());
    }
    const std::vector<f32> s = steps(w, b, 3);
    INFO("cruise " << top << ", then " << shown(s));
    CHECK(std::abs(s[0] - (top - 0.02f)) < 1e-3f);
    CHECK(std::abs(s[1] - (top - 0.04f)) < 1e-3f);
}

TEST_CASE("A unit given a reclaim in reach brakes along the one spline its reclaim's start made",
          "[order-switch]") {
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    w.issue(b, CommandType::Move, 110.5f, 60.5f, true);
    const f32 top = w.cruise(b);
    REQUIRE(top > 0.19f);
    const osc::sim::Prop* tree = w.tree(std::round(b.position().x) + 4.0f, 62.0f);
    w.issue(b, CommandType::Reclaim, tree->position().x, tree->position().z, true,
            tree->entity_id());
    w.sim.tick();
    REQUIRE(b.navigator().has_spline());
    size_t at = b.navigator().spline().current();
    for (int t = 0; t < 4; ++t) {
        w.sim.tick();
        CAPTURE(t);
        REQUIRE(b.navigator().has_spline());
        CHECK(b.navigator().spline().current() == at + 1);
        at = b.navigator().spline().current();
    }
}

TEST_CASE("A unit that moved on a patrol or a guard brakes as a new move takes its place",
          "[order-switch]") {
    const CommandType old = GENERATE(CommandType::Patrol, CommandType::Guard);
    CAPTURE(static_cast<int>(old));
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    if (old == CommandType::Guard) {
        const Unit& far = *w.make("walker", 80.5f, 60.5f);
        b.add_command_cap("RULEUCC_Guard");
        w.issue(b, CommandType::Guard, far.position().x, far.position().z, true, far.entity_id());
    } else {
        w.issue(b, CommandType::Patrol, 110.5f, 60.5f, true);
    }
    f32 top = 0.0f;
    osc::test::tick_until([&] { top = w.step(b); }, "the unit moves at its top speed", 200,
                          [&] { return top > 0.19f; });
    w.issue(b, CommandType::Move, 100.0f, 70.0f, true);
    const std::vector<f32> s = steps(w, b, 3);
    INFO("cruise " << top << ", then " << shown(s));
    CHECK(std::abs(s[0] - (2.0f * top - 0.06f)) < 1e-3f);
    CHECK(std::abs(s[1] - (top - 0.06f)) < 1e-3f);
}

TEST_CASE("A unit walking to its work brakes a node a tick from its order's step as the work is "
          "moved; moving a later order leaves the walk going",
          "[order-switch]") {
    struct Case {
        const char* what;
        CommandType type;
        bool head;
    };
    const Case c = GENERATE(Case{"its build", CommandType::BuildMobile, true},
                            Case{"its reclaim", CommandType::Reclaim, true},
                            Case{"the build after its own", CommandType::BuildMobile, false});
    CAPTURE(c.what);
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    if (c.type == CommandType::Reclaim) {
        const osc::sim::Prop* far = w.tree(110.0f, 60.0f);
        w.issue(b, c.type, 110.0f, 60.0f, true, far->entity_id());
    } else {
        w.issue(b, c.type, 110.0f, 60.0f, true);
    }
    if (!c.head) {
        w.issue(b, CommandType::BuildMobile, 116.0f, 70.0f, false);
    }
    const f32 top = w.cruise(b);
    REQUIRE(top > 0.19f);
    const f32 x = std::round(b.position().x) + 4.0f;
    osc::sim::SimCallbackEntry cb;
    cb.func_name = osc::sim::kSetCommandTargetCallback;
    cb.args["Command"] = static_cast<double>(b.command_queue()[c.head ? 0 : 1].command_id);
    if (c.type == CommandType::Reclaim) {
        cb.args["Target"] = static_cast<double>(w.tree(x, 62.0f)->entity_id().key());
    } else {
        cb.args["X"] = static_cast<double>(x);
        cb.args["Y"] = 0.0;
        cb.args["Z"] = 64.0;
    }
    cb.unit_ids = {b.entity_id()};
    w.sim.submit_callback(cb);
    const std::vector<f32> s = steps(w, b, 3);
    INFO("cruise " << top << ", then " << shown(s));
    if (c.head) {
        CHECK(std::abs(s[0] - (top - 0.02f)) < 1e-3f);
        CHECK(std::abs(s[1] - (top - 0.04f)) < 1e-3f);
    } else {
        CHECK(std::abs(s[0] - top) < 1e-3f);
        CHECK(std::abs(s[1] - top) < 1e-3f);
    }
}

TEST_CASE("A move with the id of an earlier order whose walk was aborted goes on its way",
          "[order-switch]") {
    const bool same_id = GENERATE(true, false);
    CAPTURE(same_id);
    World w;
    Unit& b = *w.make("builder", 20.5f, 60.5f);
    const osc::sim::Prop* tree = w.tree(40.0f, 60.0f);
    UnitCommand reclaim;
    reclaim.type = CommandType::Reclaim;
    reclaim.target_pos = tree->position();
    reclaim.target_id = tree->entity_id();
    reclaim.command_id = 77;
    w.sim.route_command({b.entity_id()}, reclaim, true);
    osc::test::tick_until([&] { w.sim.tick(); }, "the reclaim is done", 600,
                          [&] { return b.command_queue().empty(); });
    REQUIRE(b.position().x > 30.0f);
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {100.0f, 10.0f, 60.5f};
    move.command_id = same_id ? 77 : 78;
    w.sim.route_command({b.entity_id()}, move, true);
    const f32 from = b.position().x;
    for (int t = 0; t < 10; ++t) {
        w.sim.tick();
    }
    CHECK(b.command_queue().size() == 1);
    CHECK(b.position().x > from + 0.5f);
}
