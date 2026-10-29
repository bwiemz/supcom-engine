// brain:BuildStructure as Moho's CAiBrain::BuildStructure (retail probe build-structure).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::sim::CommandType;
using osc::sim::Unit;

namespace {

struct World {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    Unit* engineer = nullptr;

    explicit World(std::vector<osc::u16> heights = std::vector<osc::u16>(65 * 65, 1000),
                   osc::f32 water = 0.0f) {
        osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), water, water > 0.0f));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.get_army(0)->set_start_position({20.0f, 0.0f, 20.0f});
        sim.add_resource_deposit({30.5f, 0, 30.5f, 1.0f, osc::sim::ResourceDeposit::Mass});
        engineer = spawn("uel0105", 40.0f, 40.0f);
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_sim_bindings(lua, sim);
        REQUIRE(lua.do_string(R"(
            __blueprints = {
                uel0105 = {
                    Economy = { BuildableCategory = { 'ueb1101', 'ueb1103', 'xeb0010' } },
                },
                ueb1101 = { Footprint = { SizeX = 2, SizeZ = 2 } },
                ueb1103 = { Footprint = { SizeX = 2, SizeZ = 2 },
                            Physics = { BuildRestriction = 'RULEUBR_OnMassDeposit' } },
                ueb1301 = { Footprint = { SizeX = 4, SizeZ = 4 } },
                xeb0010 = { Footprint = { SizeX = 10, SizeZ = 10 } },
            }
        )"));
        global("brain", sim.get_army(0));
        global("engineer", static_cast<osc::sim::Entity*>(engineer));
    }

    Unit* block(osc::f32 x, osc::f32 z) {
        Unit* u = spawn("ueb1101", x, z);
        u->add_category("STRUCTURE");
        u->set_footprint_size(2.0f, 2.0f);
        return u;
    }

    Unit* spawn(const char* bp, osc::f32 x, osc::f32 z) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_blueprint_id(bp);
        u->set_position({x, 7.8125f, z});
        auto* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        return raw;
    }

    void global(const char* name, void* object) {
        lua_State* L = lua.raw();
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, object);
        lua_rawset(L, -3);
        lua_setglobal(L, name);
    }

    const std::deque<osc::sim::UnitCommand>& order(const std::string& args) {
        const auto r =
            lua.do_string("moho.aibrain_methods.BuildStructure(brain, engineer, " + args + ")");
        INFO(args);
        REQUIRE(r);
        return engineer->command_queue();
    }
};

} // namespace

TEST_CASE("BuildStructure gives no order Moho refuses", "[brain][build]") {
    struct Row {
        const char* args;
        bool ordered;
    };
    const std::vector<Row> rows = {
        {"'ueb1101', {30, 40, 0}, false", true},     {"'ueb1301', {30, 40, 0}, false", false},
        {"'ueb1101', {30, 8, 0}, false", true},      {"'ueb1101', {30, 7, 0}, false", false},
        {"'ueb1101', {8, 30, 0}, false", true},      {"'ueb1101', {7, 30, 0}, false", false},
        {"'ueb1101', {30, 56, 0}, false", true},     {"'ueb1101', {30, 57, 0}, false", false},
        {"'ueb1101', {56, 30, 0}, false", true},     {"'ueb1101', {57, 30, 0}, false", false},
        {"'ueb1103', {30.5, 30.5, 0}, false", true}, {"'ueb1103', {31.5, 30.5, 0}, false", false},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.args) {
            World w;
            const auto& queue = w.order(row.args);
            CHECK(queue.size() == (row.ordered ? 1u : 0u));
        }
    }
}

TEST_CASE("BuildStructure gives no order for an extractor on a taken deposit", "[brain][build]") {
    World w;
    Unit* mex = w.spawn("ueb1103", 30.5f, 30.5f);
    mex->add_category("STRUCTURE");
    mex->set_footprint_size(2.0f, 2.0f);
    CHECK(w.order("'ueb1103', {30.5, 30.5, 0}, false").empty());
    CHECK(w.order("'ueb1101', {30, 44, 0}, false").size() == 1);
}

TEST_CASE("BuildStructure places a relative site from the army's start", "[brain][build]") {
    const bool relative = GENERATE(true, false);
    World w;
    const auto& queue =
        w.order(relative ? "'ueb1101', {10, 20, 0}, true" : "'ueb1101', {10, 20, 0}, false");
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().type == CommandType::BuildMobile);
    CHECK(queue.front().target_pos.x == (relative ? 30.0f : 10.0f));
    CHECK(queue.front().target_pos.z == (relative ? 40.0f : 20.0f));
}

TEST_CASE("BuildStructure returns its last argument, whatever it orders", "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string(R"(
        local function ret(...)
            return tostring(moho.aibrain_methods.BuildStructure(unpack(arg)))
        end
        local results = {
            ret(brain, engineer, 'ueb1101', {10, 6, 0}, true),
            ret(brain, engineer, 'ueb1101', {30, 40, 0}, false),
            ret(brain, engineer, 'ueb1101', {30, 40, 0}, nil),
            ret(brain, engineer, 'ueb1301', {10, 6, 0}, true),
            ret(brain, engineer, 'ueb1301', {30, 40, 0}, false),
        }
        assert(results[1] == 'true', 'relative, ordered: ' .. results[1])
        assert(results[2] == 'false', 'absolute, ordered: ' .. results[2])
        assert(results[3] == 'nil', 'no relative: ' .. results[3])
        assert(results[4] == 'true', 'relative, refused: ' .. results[4])
        assert(results[5] == 'false', 'absolute, refused: ' .. results[5])
        local place = {30, 40, 0}
        local four = moho.aibrain_methods.BuildStructure(brain, engineer, 'ueb1101', place)
        assert(rawequal(four, place), 'four arguments: the place')
    )"));
}

TEST_CASE("BuildStructure builds on the first free site of Moho's rings about a taken one",
          "[brain][build]") {
    struct Row {
        const char* bp;
        std::vector<std::pair<osc::f32, osc::f32>> taken;
        osc::f32 x, z;
    };
    const std::vector<Row> rows = {
        {"ueb1101", {{30, 40}}, 22, 32},
        {"ueb1101", {{30, 40}, {22, 32}}, 22, 40},
        {"xeb0010", {{30, 40}}, 20, 30},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.bp << " with " << row.taken.size() << " taken") {
            World w;
            for (const auto& [x, z] : row.taken) {
                w.block(x, z);
            }
            const auto& queue = w.order(std::string("'") + row.bp + "', {30, 40, 0}, false");
            REQUIRE(queue.size() == 1);
            CHECK(queue.front().target_pos.x == row.x);
            CHECK(queue.front().target_pos.z == row.z);
        }
    }
}

TEST_CASE("BuildStructure gives no order where no ring within 900 probes has room",
          "[brain][build]") {
    const bool island = GENERATE(false, true);
    std::vector<osc::u16> heights(65 * 65, 1000);
    if (island) {
        for (int z = 20; z <= 28; ++z) {
            for (int x = 10; x <= 18; ++x) {
                heights[static_cast<size_t>(z) * 65 + static_cast<size_t>(x)] = 2000;
            }
        }
    }
    World w(std::move(heights), 10.0f);
    const auto& queue = w.order("'ueb1101', {30, 40, 0}, false");
    if (!island) {
        CHECK(queue.empty());
        return;
    }
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().target_pos.x == 14.0f);
    CHECK(queue.front().target_pos.z == 24.0f);
}

TEST_CASE("BuildStructure's edge band follows the playable area, or the whole map's for its army",
          "[brain][build]") {
    struct Row {
        bool playable;
        bool whole_map;
        osc::f32 z;
        bool ordered;
    };
    const std::vector<Row> rows = {
        {true, false, 23, false}, {true, false, 24, true},  {false, true, 56, false},
        {false, true, 55, true},  {false, false, 56, true},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION("playable " << row.playable << ", whole map " << row.whole_map << ", z "
                                    << row.z) {
            World w;
            if (row.playable) {
                w.sim.set_playable_rect(16.0f, 16.0f, 48.0f, 48.0f);
            }
            w.sim.get_army(0)->set_use_whole_map(row.whole_map);
            const std::string z = std::to_string(static_cast<int>(row.z));
            const auto& queue = w.order("'ueb1101', {30, " + z + ", 0}, false");
            CHECK(queue.size() == (row.ordered ? 1u : 0u));
        }
    }
}

TEST_CASE("BuildStructure gives the order its place's angle", "[brain][build]") {
    const int angle = GENERATE(0, 90);
    World w;
    const auto& queue = w.order("'ueb1101', {30, 40, " + std::to_string(angle) + "}, false");
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().has_facing == (angle != 0));
    CHECK(queue.front().facing == static_cast<osc::f32>(angle));
}
