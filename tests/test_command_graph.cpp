#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "renderer/command_graph_renderer.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <cmath>

using osc::renderer::command_graph_key;
using osc::renderer::command_graph_style;
using osc::renderer::command_strip;

TEST_CASE("An order's style is its own, then its inherit_from's, then default's",
          "[renderer][command_graph]") {
    osc::lua::LuaState lua;
    // A cut of retail's commandgraphparams.lua
    REQUIRE(lua.do_string(R"(
        CommandGraphParams = {
            default = {
                orderline_texture = '/orderline_generic.dds',
                orderline_uv_aspect_ratio = 1.0,
                orderline_color = '3300ffff',
                orderline_selected_color = 'dd00ffff',
                waypoint_selected_color = '88ffffff',
            },
            default_AttackColors = {
                orderline_color = '33ff0000',
                orderline_selected_color = 'ddff0000',
            },
            UNITCOMMAND_Attack = {
                inherit_from = 'default_AttackColors',
                waypoint_texture = '/attack_btn_up.dds',
            },
            UNITCOMMAND_Tactical = { inherit_from = 'UNITCOMMAND_Attack' },
            UNITCOMMAND_Move = { waypoint_texture = '/move_btn_up.dds' },
            UNITCOMMAND_Patrol = {
                waypoint_texture = '/patrol_btn_up.dds',
                orderline_texture = '/orderline_arrow02.dds',
                orderline_anim_rate = 0.25,
            },
        }
    )")
                .ok());
    lua_State* L = lua.raw();
    lua_getglobal(L, "CommandGraphParams");
    const int params = lua_gettop(L);

    const auto move = command_graph_style(L, params, "UNITCOMMAND_Move");
    CHECK(move.line_texture == "/orderline_generic.dds");
    CHECK(move.waypoint_texture == "/move_btn_up.dds");
    CHECK(move.line_selected_color[0] == Catch::Approx(0.0f));
    CHECK(move.line_selected_color[1] == Catch::Approx(1.0f));
    CHECK(move.line_selected_color[2] == Catch::Approx(1.0f));
    CHECK(move.line_selected_color[3] == Catch::Approx(0xdd / 255.0f));

    const auto tactical = command_graph_style(L, params, "UNITCOMMAND_Tactical");
    CHECK(tactical.waypoint_texture == "/attack_btn_up.dds");
    CHECK(tactical.line_selected_color[0] == Catch::Approx(1.0f));
    CHECK(tactical.line_selected_color[1] == Catch::Approx(0.0f));
    CHECK(tactical.line_texture == "/orderline_generic.dds");
    CHECK(tactical.waypoint_selected_color[3] == Catch::Approx(0x88 / 255.0f));

    const auto patrol = command_graph_style(L, params, "UNITCOMMAND_Patrol");
    CHECK(patrol.line_texture == "/orderline_arrow02.dds");
    CHECK(patrol.anim_rate == Catch::Approx(0.25f));
    CHECK(patrol.line_color[2] == Catch::Approx(1.0f));
    CHECK(lua_gettop(L) == params);
    lua_pop(L, 1);
}

TEST_CASE("An order's kind names its CommandGraphParams entry; some draw none",
          "[renderer][command_graph]") {
    using osc::sim::CommandType;
    CHECK(command_graph_key(CommandType::Move) == "UNITCOMMAND_Move");
    CHECK(command_graph_key(CommandType::TransportLoad) == "UNITCOMMAND_TransportLoadUnits");
    CHECK(command_graph_key(CommandType::Overcharge) == "UNITCOMMAND_OverCharge");
    CHECK(command_graph_key(CommandType::Stop).empty());
    CHECK(command_graph_key(CommandType::Upgrade).empty());
    CHECK(command_graph_key(CommandType::BuildFactory).empty());
}

TEST_CASE("An order's leg is a strip along it, on the ground", "[renderer][command_graph]") {
    using osc::sim::Vector3;
    std::array<Vector3, 4> c;
    REQUIRE(command_strip({0, 0, 0}, {10, 0, 0}, 0.5f, c));
    CHECK(c[0].x == Catch::Approx(0.0f));
    CHECK(c[1].x == Catch::Approx(10.0f));
    CHECK(std::abs(c[0].z) == Catch::Approx(0.5f));
    CHECK(c[0].z == Catch::Approx(-c[3].z));
    CHECK(c[1].z == Catch::Approx(c[0].z));
    CHECK(c[0].y == Catch::Approx(0.0f));
    REQUIRE(command_strip({0, 0, 0}, {0, 4, 10}, 0.5f, c));
    CHECK(c[0].y == Catch::Approx(0.0f));
    CHECK(c[1].y == Catch::Approx(4.0f));
    CHECK(std::abs(c[0].x) == Catch::Approx(0.5f));
    CHECK_FALSE(command_strip({0, 0, 0}, {0, -10, 0}, 0.5f, c));
    CHECK_FALSE(command_strip({1, 2, 3}, {1, 2, 3}, 0.5f, c));
}

TEST_CASE("An order chain bends through its waypoints, as Moho's command graph draws it",
          "[renderer][command_graph]") {
    using osc::renderer::command_curve;
    using osc::sim::Vector3;
    const std::vector<Vector3> straight = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}};
    for (const Vector3& p : command_curve(straight, 0, 20, 1.0f)) {
        CHECK(p.z == Catch::Approx(0.0f).margin(1e-5));
    }
    const std::vector<Vector3> corner = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}};
    const auto leg = command_curve(corner, 0, 20, 1.0f);
    REQUIRE(leg.size() == 21);
    CHECK(leg.front().x == Catch::Approx(0.0f));
    CHECK(leg.back().x == Catch::Approx(10.0f));
    CHECK(leg.back().z == Catch::Approx(0.0f));
    CHECK(leg[15].z < -0.01f);
    CHECK(std::abs(leg[10].z) < 0.25f * 10.0f);
    CHECK(command_curve(corner, 2, 20, 1.0f).empty());
}

TEST_CASE("A build order's site is its structure's skirt", "[renderer][command_graph]") {
    using osc::renderer::build_pad;
    // An air factory: footprint 5, skirt 8 from 1.5 outside it
    const auto factory = build_pad(30.5f, 40.5f, 5, 5, 8, 8, -1.5f, -1.5f);
    CHECK(factory[0] == Catch::Approx(26.5f));
    CHECK(factory[1] == Catch::Approx(36.5f));
    CHECK(factory[2] == Catch::Approx(34.5f));
    CHECK(factory[3] == Catch::Approx(44.5f));
    // A blueprint without a skirt: its footprint
    const auto bare = build_pad(10, 10, 2, 2, 0, 0, 0, 0);
    CHECK(bare[0] == Catch::Approx(9.0f));
    CHECK(bare[2] == Catch::Approx(11.0f));
}

TEST_CASE("Shift shows the selected units' orders and the army's build sites",
          "[renderer][command_graph]") {
    osc::sim::WorldSnapshot world;
    const auto add = [&](osc::u32 id, osc::i32 army, bool is_unit) {
        osc::sim::EntityRecord e;
        e.id = id;
        e.army = army;
        e.is_unit = is_unit;
        world.entities.push_back(e);
    };
    add(1, 0, true);  // selected
    add(2, 0, true);  // the army's, not selected
    add(3, 1, true);  // another army's, selected
    add(4, 1, true);  // another army's
    add(5, 0, false); // a prop
    const std::unordered_set<osc::u32> selected{1, 3};
    std::vector<std::pair<osc::u32, bool>> got;
    for (const auto& [e, chosen] : osc::renderer::command_graph_units(world, &selected, 0)) {
        got.emplace_back(e->id, chosen);
    }
    CHECK(got == std::vector<std::pair<osc::u32, bool>>{{1, true}, {2, false}, {3, true}});
}

TEST_CASE("A structure ordered and not started is a planned site", "[renderer][command_graph]") {
    osc::sim::WorldSnapshot world;
    const auto build = [](const char* bp, osc::f32 x) {
        osc::sim::CommandRecord c;
        c.type = osc::sim::CommandType::BuildMobile;
        c.target_pos = {x, 0, 10};
        c.blueprint_id = bp;
        return c;
    };
    osc::sim::EntityRecord builder;
    builder.id = 1;
    builder.army = 0;
    builder.is_unit = true;
    builder.build_target_id = 7;
    builder.command_offset = 0;
    builder.command_count = 3;
    world.commands = {build("ueb1101", 10), build("ueb1101", 12), build("ueb0101", 20)};
    world.entities.push_back(builder);

    const auto sites = osc::renderer::planned_build_sites(world, nullptr, 0);
    REQUIRE(sites.size() == 2);
    CHECK(sites[0].blueprint == "ueb1101");
    CHECK(sites[0].position.x == Catch::Approx(12.0f));
    CHECK(sites[1].blueprint == "ueb0101");

    world.entities[0].build_target_id = 0;
    CHECK(osc::renderer::planned_build_sites(world, nullptr, 0).size() == 3);
}

TEST_CASE("A structure ordered and not yet run is planned in place of the one under way",
          "[renderer][command_graph]") {
    osc::sim::WorldSnapshot world;
    osc::sim::CommandRecord building;
    building.type = osc::sim::CommandType::BuildMobile;
    building.target_pos = {10, 0, 10};
    building.blueprint_id = "ueb1101";
    osc::sim::CommandRecord ordered = building;
    ordered.target_pos = {20, 0, 10};
    ordered.blueprint_id = "ueb0101";
    ordered.pending = true;
    osc::sim::EntityRecord builder;
    builder.id = 1;
    builder.army = 0;
    builder.is_unit = true;
    builder.build_target_id = 7;
    builder.command_count = 1;
    world.commands = {building};
    world.pending_commands = {ordered};
    world.pending_queues = {{1, 0, 1}};
    world.entities.push_back(builder);

    const auto sites = osc::renderer::planned_build_sites(world, nullptr, 0);
    REQUIRE(sites.size() == 1);
    CHECK(sites[0].blueprint == "ueb0101");
}

TEST_CASE("A structure standing on its site is started for every builder ordered to it",
          "[renderer][command_graph]") {
    osc::sim::WorldSnapshot world;
    osc::sim::CommandRecord order;
    order.type = osc::sim::CommandType::BuildMobile;
    order.target_pos = {20, 0, 20};
    order.blueprint_id = "ueb0101";
    osc::sim::EntityRecord factory;
    factory.id = 5;
    factory.army = 0;
    factory.is_unit = true;
    factory.is_being_built = true;
    factory.blueprint_id = "ueb0101";
    factory.position = {20, 0, 20};
    osc::sim::EntityRecord walking;
    walking.id = 2;
    walking.army = 0;
    walking.is_unit = true;
    walking.command_offset = 0;
    walking.command_count = 1;
    world.commands = {order};
    world.entities = {factory, walking};

    CHECK(osc::renderer::planned_build_sites(world, nullptr, 0).empty());
    world.entities[0].position = {30, 0, 20};
    CHECK(osc::renderer::planned_build_sites(world, nullptr, 0).size() == 1);
}
