// brain:BuildStructure as Moho's CAiBrain::BuildStructure (retail probe build-structure).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/dmath.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/script_class.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <deque>
#include <functional>
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
        engineer->add_category("ENGINEER");
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_sim_bindings(lua, sim);
        REQUIRE(lua.do_string(R"(
            __blueprints = {
                uel0105 = {
                    Economy = { BuildableCategory = { 'ueb1101', 'ueb1103', 'xeb0010' } },
                },
                ueb1101 = { Footprint = { SizeX = 2, SizeZ = 2 } },
                ueb1103 = { Footprint = { SizeX = 1, SizeZ = 1 },
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

    std::string answer(const std::string& expr) {
        INFO(expr);
        REQUIRE(lua.do_string("answer = tostring(" + expr + ")"));
        lua_State* L = lua.raw();
        lua_getglobal(L, "answer");
        std::string out = lua_tostring(L, -1);
        lua_pop(L, 1);
        return out;
    }

    bool can_build(const std::string& at) {
        return answer("moho.aibrain_methods.CanBuildStructureAt(brain, 'ueb1101', " + at + ")") ==
               "true";
    }

    std::string site(const std::string& expr) {
        REQUIRE(lua.do_string("site = " + expr));
        return answer("site and site[1] .. ' ' .. site[2] or site");
    }
};

void reserve(World& w, bool structure, const std::string& bp, osc::f32 x, osc::f32 z) {
    REQUIRE(w.lua.do_string("bp = '" + bp + "'"));
    REQUIRE(w.lua.do_string("x, z = " + std::to_string(x) + ", " + std::to_string(z)));
    if (structure) {
        REQUIRE(w.lua.do_string(
            "moho.aibrain_methods.BuildStructure(brain, engineer, bp, {x, z, 0}, false)"));
    } else {
        REQUIRE(w.lua.do_string("IssueBuildMobile({engineer}, {x, 0, z}, bp, {})"));
    }
}

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
    CHECK(queue.front().has_facing);
    CHECK(queue.front().facing == static_cast<osc::f32>(angle));
}

TEST_CASE("A structure built to an order's angle faces Moho's quaternion of it, zeros signed",
          "[brain][build]") {
    const osc::f32 angle = GENERATE(0.0f, 90.0f);
    CAPTURE(angle);
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    osc::lua::register_moho_bindings(state, sim);
    osc::lua::register_sim_bindings(state, sim);
    sim.add_army("ARMY_1", "ARMY_1");
    lua_State* L = state.raw();
    REQUIRE(state
                .do_string("return {BlueprintId = 'pgen', Categories = {'STRUCTURE'},"
                           " Defense = {MaxHealth = 100}, Economy = {BuildTime = 10}}")
                .ok());
    store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
    lua_settop(L, 0);
    store.expose_to_lua(L);

    lua_pushstring(L, "__osc_create_building_unit");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "pgen");
    lua_pushnumber(L, 1);
    lua_pushnumber(L, 10);
    lua_pushnil(L);
    lua_pushnumber(L, 10);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    lua_pushnumber(L, angle);
    REQUIRE(lua_pcall(L, 8, 2, 0) == 0);
    auto* pgen = sim.entity_registry().find(osc::sim::to_entity_key(L, -2));
    lua_settop(L, 0);
    REQUIRE(pgen);
    const osc::f32 half = angle * 0.017453292f * -0.5f;
    const osc::f32 s = osc::dmath::sin(half);
    const auto q = pgen->orientation();
    CHECK(q.y == s);
    CHECK(q.w == osc::dmath::cos(half));
    CHECK(q.x == 0.0f);
    CHECK(q.z == 0.0f);
    CHECK(std::signbit(q.x) == std::signbit(s));
    CHECK(std::signbit(q.y) == std::signbit(s));
    CHECK(std::signbit(q.z) == std::signbit(s));
}

TEST_CASE("CanBuildStructureAt refuses the site of a BuildStructure order, not of other orders",
          "[brain][build]") {
    const bool structure = GENERATE(true, false);
    World w;
    reserve(w, structure, "ueb1101", 30, 40);
    REQUIRE(w.engineer->command_queue().size() == 1);
    CHECK(w.can_build("{30, 0, 40}") == !structure);
    CHECK(w.can_build("{31, 0, 41}") == !structure);
    CHECK(w.can_build("{30, 0, 50}"));
}

TEST_CASE("A BuildStructure reservation lasts while its builder lives and has the order",
          "[brain][build]") {
    struct Row {
        const char* name;
        std::function<void(World&, Unit*)> then;
        bool free;
    };
    const std::vector<Row> rows = {
        {"kept", [](World&, Unit*) {}, false},
        {"orders cleared", [](World& w, Unit*) { w.engineer->clear_commands(); }, true},
        {"builder dying", [](World& w, Unit*) { w.engineer->begin_dying(); }, true},
        {"builder destroyed", [](World& w, Unit*) { w.engineer->mark_destroyed(); }, true},
        {"a second order of the cell kept",
         [](World& w, Unit*) {
             REQUIRE(w.lua.do_string("moho.aibrain_methods.BuildStructure(brain, other, "
                                     "'ueb1101', {30.3, 40.3, 0}, false)"));
         },
         false},
        {"a second order of the cell, its builder dying",
         [](World& w, Unit* other) {
             REQUIRE(w.lua.do_string("moho.aibrain_methods.BuildStructure(brain, other, "
                                     "'ueb1101', {30.3, 40.3, 0}, false)"));
             REQUIRE(other->command_queue().size() == 1);
             other->begin_dying();
         },
         true},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.name) {
            World w;
            Unit* other = w.spawn("uel0105", 44.0f, 40.0f);
            w.global("other", static_cast<osc::sim::Entity*>(other));
            REQUIRE(w.order("'ueb1101', {30, 40, 0}, false").size() == 1);
            row.then(w, other);
            CHECK(w.can_build("{30, 0, 40}") == row.free);
        }
    }
}

TEST_CASE("FindPlaceToBuild with an alliance takes a site its side's structures or the ground "
          "refuse, as Moho's CanBuildStructureAt",
          "[brain][build]") {
    struct Row {
        const char* what;
        int holder;
        bool water;
        const char* alliance;
        bool taken;
    };
    const Row rows[] = {
        {"our own structure, none asked", 0, false, "nil", false},
        {"our own structure, Ally", 0, false, "'Ally'", true},
        {"our own structure, Enemy", 0, false, "'Enemy'", false},
        {"our own structure, Neutral", 0, false, "'Neutral'", false},
        {"an enemy's structure, none asked", 1, false, "nil", false},
        {"an enemy's structure, Ally", 1, false, "'Ally'", false},
        {"an enemy's structure, Enemy", 1, false, "'Enemy'", true},
        {"under water, none asked", -1, true, "nil", false},
        {"under water, Ally", -1, true, "'Ally'", true},
        {"under water, Enemy", -1, true, "'Enemy'", true},
    };
    for (const Row& row : rows) {
        DYNAMIC_SECTION(row.what) {
            World w(std::vector<osc::u16>(65 * 65, 1000), row.water ? 10.0f : 0.0f);
            w.sim.add_army("ARMY_2", "ARMY_2");
            w.sim.set_alliance(0, 1, osc::sim::Alliance::Enemy);
            if (row.holder >= 0) {
                w.block(30.0f, 40.0f)->set_army(row.holder);
            }
            CHECK(w.answer(std::string("moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', ") +
                           "'ueb1101', {{{'OscSite'}, {30, 40, 0}}}, false, nil, " + row.alliance +
                           ") ~= nil") == (row.taken ? "true" : "false"));
        }
    }
}

TEST_CASE("FindPlaceToBuild answers with the template's own entry, or nil", "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string("site_list = {{{'OscSite'}, {30, 40, 9}}}"));
    CHECK(w.answer("rawequal(moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', 'ueb1101', "
                   "site_list, false, nil), site_list[1][2])") == "true");
    w.block(30.0f, 40.0f);
    CHECK(w.answer("moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', 'ueb1101', site_list, "
                   "false, nil)") == "nil");
}

TEST_CASE("FindPlaceToBuild keeps a template site's skirt off a deposit's cells, with an alliance "
          "too",
          "[brain][build]") {
    World w;
    CHECK(w.answer("moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', 'ueb1101', "
                   "{{{'OscSite'}, {31, 31, 0}}}, false, nil, 'Ally')") == "nil");
}

TEST_CASE("FindPlaceToBuild's resource answer is the middle of the deposit's cells",
          "[brain][build]") {
    World w;
    w.sim.add_resource_deposit({22.9f, 0, 35.9f, 1.0f, osc::sim::ResourceDeposit::Mass});
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'T1Resource', 'ueb1103', {}, false, "
                 "nil, nil, 22, 35)") == "22.5 35.5");
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'T1Resource', 'ueb1103', {}, true, "
                 "nil, nil, 22, 35)") == "0.5 0.5");
}

TEST_CASE("FindPlaceToBuild starts from the override only when both its coordinates are given",
          "[brain][build]") {
    World w;
    w.sim.add_resource_deposit({22.9f, 0, 35.9f, 1.0f, osc::sim::ResourceDeposit::Mass});
    const auto from = [&](const std::string& override_xz) {
        return w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'T1Resource', 'ueb1103', {}, "
                      "true, nil, nil, " +
                      override_xz + ")");
    };
    CHECK(from("22, 35") == "0.5 0.5");
    CHECK(from("22, nil") == "10.5 10.5");
    CHECK(from("nil, 35") == "10.5 10.5");
}

TEST_CASE("FindPlaceToBuild matches its type to a template's names in any case", "[brain][build]") {
    World w;
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', 'ueb1101', "
                 "{{{'oscSITE'}, {30, 40, 0}}}, false, nil)") == "30 40");
}

TEST_CASE("FindPlaceToBuild adds the builder's height to every distance, as Moho",
          "[brain][build]") {
    const bool high = GENERATE(false, true);
    World w;
    w.engineer->set_position({40.0f, high ? 1000.0f : 7.8125f, 40.0f});
    w.engineer->place_now();
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', 'ueb1101', "
                 "{{{'OscSite'}, {41, 41, 0}, {41.41, 40, 0}}}, false, engineer)") ==
          (high ? "41 41" : "41.41 40"));
}

TEST_CASE("FindPlaceToBuild raises Moho's errors for its arguments", "[brain][build]") {
    struct Row {
        const char* args;
        const char* error;
    };
    const Row rows[] = {
        {"'OscSite', 'ueb1101', sites", "expected between 6 and 10 args, but got 4"},
        {"'OscSite', 'ueb1101', sites, false", "expected between 6 and 10 args, but got 5"},
        {"'OscSite', 'ueb1101', sites, false, nil, nil, 1, 2, 3, 4",
         "expected between 6 and 10 args, but got 11"},
        {"{}, 'ueb1101', sites, false, nil", "Invalid Type name passed in"},
        {"5, 'ueb1101', sites, false, nil", "Invalid Type name passed in"},
        {"'OscSite', nil, sites, false, nil", "Invalid Structure name passed in"},
        {"'OscSite', 5, sites, false, nil", "Invalid Structure name passed in"},
        {"'OscSite', 'ueb1101', nil, false, nil",
         "Invalid buildingTypes passed in. Not a lua table!"},
        {"'OscSite', 'ueb1101', 'x', false, nil",
         "Invalid buildingTypes passed in. Not a lua table!"},
        {"'OscSite', 'ueb1101', sites, false, nil, 'Bogus'", "Invalid enum value Bogus"},
        {"'OscSite', 'ueb1101', sites, false, {}", "Expected a game object."},
        {"'OscSite', 'ueb1101', sites, 1, nil", "attempt to use as boolean a number value"},
        {"'OscSite', 'ueb1101', sites, false, nil, nil, 'x', 40",
         "attempt to use as number a string value"},
        {"'OscSite', 'ueb1101', sites, false, nil, nil, 30, '40'",
         "attempt to use as number a string value"},
        {"'OscSite', 'ueb1101', sites, false, nil, nil, 30, 40, '5'",
         "attempt to use as integer a string value"},
        {"'OscSite', 'ueb1101', sites, nil, nil", ""},
        {"'OscSite', 'ueb1101', sites, false, nil, nil, 'x', nil", ""},
        {"'OscSite', 'ueb1101', {5}, false, nil", "attempt to index a number value"},
        {"'OscSite', 'ueb1101', {{'OscSite', {30, 40, 0}}}, false, nil",
         "Error parsing building types. Missing type match lua table!"},
        {"'OscSite', 'ueb1101', {{{5}, {30, 40, 0}}}, false, nil",
         "attempt to get as string a number value"},
        {"'OscSite', 'ueb1101', {{{'OscSite'}, {'a', 40, 0}}}, false, nil",
         "attempt to get as number a string value"},
        {"'OscSite', 'ueb1101', {{{'OscSite'}, {30}}}, false, nil",
         "attempt to get as number a nil value"},
        {"'OscSite', 'ueb1101', {{{'X'}, 5}, sites[1]}, false, nil", ""},
        {"'OscSite', 'ueb1101', sites, false, nil, 12", ""},
        {"'OscSite', 'ueb1101', sites, false, engineer", ""},
    };
    for (const Row& row : rows) {
        DYNAMIC_SECTION(row.args) {
            World w;
            REQUIRE(w.lua.do_string("sites = {{{'OscSite'}, {30, 40, 0}}}"));
            const std::string error = w.answer(std::string("(function() local ok, e = pcall(") +
                                               "moho.aibrain_methods.FindPlaceToBuild, brain, " +
                                               row.args + ") return ok and '' or e end)()");
            if (*row.error == '\0') {
                CHECK(error.empty());
            } else {
                CHECK(error.find(row.error) != std::string::npos);
            }
        }
    }
}

TEST_CASE("FindPlaceToBuild with Enemy takes a deposit an enemy's extractor holds",
          "[brain][build]") {
    const bool enemy = GENERATE(true, false);
    World w;
    w.sim.add_resource_deposit({40.5f, 0, 20.5f, 1.0f, osc::sim::ResourceDeposit::Mass});
    w.sim.add_army("ARMY_2", "ARMY_2");
    w.sim.set_alliance(0, 1, osc::sim::Alliance::Enemy);
    w.block(30.5f, 30.5f)->set_army(1);
    const std::string alliance = enemy ? "'Enemy'" : "nil";
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'T1Resource', 'ueb1103', {}, false, "
                 "nil, " +
                 alliance + ")") == (enemy ? "30.5 30.5" : "40.5 20.5"));
}

TEST_CASE("FindPlaceToBuild takes tied deposits in the order Moho's sort leaves them",
          "[brain][build]") {
    World w;
    w.block(30.5f, 30.5f);
    std::vector<std::pair<osc::f32, osc::f32>> far;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            const osc::f32 x = 8.5f + 8.0f * static_cast<osc::f32>(i);
            const osc::f32 z = 8.5f + 9.0f * static_cast<osc::f32>(j);
            if ((x - 32.0f) * (x - 32.0f) + (z - 32.0f) * (z - 32.0f) > 60.0f) {
                far.emplace_back(x, z);
            }
        }
    }
    REQUIRE(far.size() == 34);
    const auto add = [&](osc::f32 x, osc::f32 z) {
        w.sim.add_resource_deposit({x, 0, z, 1.0f, osc::sim::ResourceDeposit::Mass});
    };
    for (size_t i = 0; i < far.size(); ++i) {
        if (i == 3) {
            add(27.5f, 27.5f);
            add(36.5f, 27.5f);
            add(27.5f, 36.5f);
            add(36.5f, 36.5f);
        }
        add(far[i].first, far[i].second);
    }
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'T1Resource', 'ueb1103', {}, false, "
                 "nil, nil, 32, 32)") == "36.5 36.5");
}

TEST_CASE("FindPlaceToBuild and CreateResourceBuildingNearest pass over BuildStructure's sites",
          "[brain][build]") {
    const bool structure = GENERATE(true, false);
    World w;
    w.sim.add_resource_deposit({50.5f, 0, 30.5f, 1.0f, osc::sim::ResourceDeposit::Mass});
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    reserve(w, structure, "ueb1101", 30, 40);
    reserve(w, structure, "ueb1103", 30.5f, 30.5f);
    REQUIRE(w.engineer->command_queue().size() == 2);
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'OscSite', 'ueb1101', "
                 "{{{'OscSite'}, {30, 40, 0}, {30, 52, 0}}}, false, engineer)") ==
          (structure ? "30 52" : "30 40"));
    const std::string mass = structure ? "50.5 30.5" : "30.5 30.5";
    CHECK(w.site("moho.aibrain_methods.FindPlaceToBuild(brain, 'T1Resource', 'ueb1103', {}, false, "
                 "engineer)") == mass);
    CHECK(w.site("moho.aibrain_methods.CreateResourceBuildingNearest(brain, 'ueb1103', 20, 20)") ==
          mass);
}

TEST_CASE("CreateResourceBuildingNearest puts the building on the middle of the deposit's cells",
          "[brain][build]") {
    World w;
    w.sim.add_resource_deposit({40.4f, 0, 20.6f, 1.0f, osc::sim::ResourceDeposit::Mass});
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    CHECK(w.site("moho.aibrain_methods.CreateResourceBuildingNearest(brain, 'ueb1103', 40, 20)") ==
          "40.5 20.5");
}

TEST_CASE("An extractor stands on a deposit whose cells it covers, rounded from its point",
          "[brain][build]") {
    World w;
    w.sim.add_resource_deposit({45.0f, 0, 45.0f, 1.0f, osc::sim::ResourceDeposit::Mass});
    const auto at = [&](const char* where) {
        return w.answer(std::string("moho.aibrain_methods.CanBuildStructureAt(brain, 'ueb1103', ") +
                        where + ")");
    };
    CHECK(at("{44.5, 0, 44.5}") == "true");
    CHECK(at("{45.5, 0, 45.5}") == "false");
}

TEST_CASE("CreateResourceBuildingNearest takes tied deposits in the order Moho's sort leaves them",
          "[brain][build]") {
    World w;
    w.block(30.5f, 30.5f);
    std::vector<std::pair<osc::f32, osc::f32>> far;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            const osc::f32 x = 8.5f + 8.0f * static_cast<osc::f32>(i);
            const osc::f32 z = 8.5f + 9.0f * static_cast<osc::f32>(j);
            if ((x - 32.0f) * (x - 32.0f) + (z - 32.0f) * (z - 32.0f) > 60.0f) {
                far.emplace_back(x, z);
            }
        }
    }
    REQUIRE(far.size() == 34);
    const auto add = [&](osc::f32 x, osc::f32 z) {
        w.sim.add_resource_deposit({x, 0, z, 1.0f, osc::sim::ResourceDeposit::Mass});
    };
    for (size_t i = 0; i < far.size(); ++i) {
        if (i == 3) {
            add(27.5f, 27.5f);
            add(36.5f, 27.5f);
            add(27.5f, 36.5f);
            add(36.5f, 36.5f);
        }
        add(far[i].first, far[i].second);
    }
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    CHECK(w.site("moho.aibrain_methods.CreateResourceBuildingNearest(brain, 'ueb1103', 32, 32)") ==
          "36.5 36.5");
}

TEST_CASE("The sim's placement check refuses ground cells the occupancy grid holds, the UI's not",
          "[brain][build]") {
    namespace occupancy = osc::blueprints::occupancy;
    World w;
    const auto sim_answer = [&] {
        return w.answer(
            "moho.aibrain_methods.CanBuildStructureAt(brain, 'ueb1103', {30.5, 0, 30.5})");
    };
    CHECK(sim_answer() == "true");
    w.sim.occupy_cells(occupancy::kWater, {30, 30, 31, 31}, true);
    CHECK(sim_answer() == "true");
    w.sim.occupy_cells(occupancy::kLand | occupancy::kSeabed, {30, 30, 31, 31}, true);
    CHECK(sim_answer() == "false");
    CHECK(osc::lua::can_build_structure(w.lua.raw(), w.sim, 0, "ueb1103", 30.5f, 30.5f));
}

TEST_CASE("CreateResourceBuildingNearest passes over a deposit whose cell the occupancy grid holds",
          "[brain][build]") {
    namespace occupancy = osc::blueprints::occupancy;
    World w;
    w.sim.add_resource_deposit({36.5f, 0, 30.5f, 1.0f, osc::sim::ResourceDeposit::Mass});
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    w.sim.occupy_cells(occupancy::kLand | occupancy::kSeabed, {30, 30, 31, 31}, true);
    CHECK(w.site("moho.aibrain_methods.CreateResourceBuildingNearest(brain, 'ueb1103', 30, 30)") ==
          "36.5 30.5");
}

TEST_CASE("Moho's func_LocationIsFree keeps a land structure out of the water", "[brain][build]") {
    const bool water = GENERATE(true, false);
    World w(std::vector<osc::u16>(65 * 65, 1000), water ? 10.0f : 0.0f);
    const osc::sim::StructurePlacement placement(w.sim, 0, [&w](const std::string& bp) {
        return osc::sim::structure_rules(w.lua.raw(), w.sim, bp);
    });
    CHECK(placement.location_free("ueb1101", 30.0f, 40.0f) == !water);
}

TEST_CASE("CreateUnitNearSpot puts a unit on a site BuildStructure has ordered", "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    REQUIRE(w.order("'ueb1101', {30, 40, 0}, false").size() == 1);
    CHECK(w.site("moho.aibrain_methods.CreateUnitNearSpot(brain, 'ueb1101', 30, 40)") == "30 40");
}

TEST_CASE("CreateUnitNearSpot looks past the cells 5 either way of the start, rounded",
          "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end) "
        "__blueprints.ueb0101 = { Footprint = { SizeX = 5, SizeZ = 5 } }"));
    w.sim.get_army(0)->set_start_position({29.5f, 0.0f, 28.5f});
    CHECK(w.site("moho.aibrain_methods.CreateUnitNearSpot(brain, 'ueb0101', 29.5, 28.5)") ==
          "22.5 21.5");
}

TEST_CASE("CreateUnitNearSpot keeps a cell between its site's skirt and another structure's",
          "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    Unit* other = w.block(12.0f, 10.0f);
    other->set_skirt(2.0f, 2.0f, 0.0f, 0.0f);
    other->set_collision_shape(
        {osc::sim::CollisionShapeType::BOX, 0.0f, 0.5f, 0.0f, 1.0f, 0.5f, 1.0f});
    CHECK(w.site("moho.aibrain_methods.CreateUnitNearSpot(brain, 'ueb1101', 14, 10)") == "15 9");
}

TEST_CASE("CreateUnitNearSpot leaves the start's cells clear, those another structure held too",
          "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    Unit* held = w.block(20.0f, 20.0f);
    w.sim.occupy_ground(held->entity_id(), {osc::blueprints::occupancy::kLand, {{19, 19, 21, 21}}});
    REQUIRE(w.sim.occupancy().ground_at(19, 19));
    CHECK(w.site("moho.aibrain_methods.CreateUnitNearSpot(brain, 'ueb1101', 30, 40)") == "30 40");
    CHECK_FALSE(w.sim.occupancy().ground_at(19, 19));
    CHECK_FALSE(w.sim.occupancy().ground_at(15, 15));
}

TEST_CASE("CreateUnitNearSpot places nothing about a spot within a cell of the map's edge",
          "[brain][build]") {
    World w;
    REQUIRE(w.lua.do_string(
        "rawset(_G, 'CreateUnitHPR', function(bp, army, x, y, z) return {x, z} end)"));
    CHECK(w.site("moho.aibrain_methods.CreateUnitNearSpot(brain, 'ueb1101', 0.5, 30)") == "nil");
    CHECK(w.site("moho.aibrain_methods.CreateUnitNearSpot(brain, 'ueb1101', 1.5, 30)") == "1 30");
}

TEST_CASE("A brain's BuildStructure reservations survive a save", "[brain][build][save]") {
    World w;
    REQUIRE(w.order("'ueb1101', {30.7, 40.2, 0}, false").size() == 1);
    REQUIRE(w.order("'ueb1301', {12, 50, 0}, false").size() == 1);
    const auto bytes = osc::sim::save_sim_state(w.sim);

    osc::lua::LuaState lua;
    osc::sim::SimState loaded(lua.raw(), nullptr);
    loaded.add_army("ARMY_1", "ARMY_1");
    const std::string error = osc::sim::load_sim_state(loaded, bytes);
    INFO(error);
    REQUIRE(error.empty());
    const auto& kept = loaded.get_army(0)->build_reservations();
    REQUIRE(kept.size() == 2);
    CHECK(kept.at({30, 40}).builder == w.engineer->entity_id());
    CHECK(kept.at({30, 40}).command == w.engineer->command_queue().front().command_id);
    CHECK(kept.at({30, 40}).command != 0);
    CHECK(kept.at({12, 50}).builder == w.engineer->entity_id());
    CHECK(kept.at({12, 50}).command == 0);
}
