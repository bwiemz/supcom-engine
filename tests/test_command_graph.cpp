#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "renderer/camera.hpp"
#include "renderer/command_graph_renderer.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <cmath>
#include <memory>
#include <vector>

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
                waypoint_highlight_color = 'ffffffff',
                waypoint_highlight_scale = 1.5,
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
    CHECK(tactical.waypoint_highlight_color[3] == Catch::Approx(1.0f));
    CHECK(tactical.waypoint_highlight_scale == Catch::Approx(1.5f));

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
    const Vector3 none{0, 0, 0};
    for (const Vector3& p : command_curve({0, 0, 0}, {10, 0, 0}, none, {1, 0, 0}, 20, 1.0f)) {
        CHECK(p.z == Catch::Approx(0.0f).margin(1e-5));
    }
    const Vector3 bisector{std::sqrt(0.5f), 0, std::sqrt(0.5f)};
    const auto leg = command_curve({0, 0, 0}, {10, 0, 0}, none, bisector, 20, 1.0f);
    REQUIRE(leg.size() == 21);
    CHECK(leg.front().x == Catch::Approx(0.0f));
    CHECK(leg.back().x == Catch::Approx(10.0f));
    CHECK(leg.back().z == Catch::Approx(0.0f));
    CHECK(leg[15].z < -0.01f);
    CHECK(std::abs(leg[10].z) < 0.25f * 10.0f);
    CHECK(command_curve({0, 0, 0}, {10, 0, 0}, none, bisector, 0, 1.0f).empty());
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

TEST_CASE("Shift draws every unit of the army's orders, a selected one's in its selected colours",
          "[renderer][command_graph]") {
    osc::sim::WorldSnapshot world;
    for (osc::u32 id : {1u, 2u}) {
        osc::sim::EntityRecord e;
        e.id = id;
        e.army = 0;
        e.is_unit = true;
        e.position = {static_cast<osc::f32>(id) * 10.0f, 0, 0};
        e.command_offset = static_cast<osc::u32>(world.commands.size());
        e.command_count = 1;
        osc::sim::CommandRecord move;
        move.type = osc::sim::CommandType::Move;
        move.target_pos = {50, 0, 50};
        world.commands.push_back(move);
        world.entities.push_back(e);
    }
    osc::renderer::CommandGraphStyle style;
    style.line_color = {0, 1, 1, 0.2f};
    style.line_selected_color = {0, 1, 1, 0.87f};
    style.waypoint_color = {1, 1, 1, 0.27f};
    style.waypoint_selected_color = {1, 1, 1, 1};
    style.waypoint_scale = 0.5f;
    const std::unordered_set<osc::u32> selected{1};
    const auto paths =
        osc::renderer::command_graph_paths(osc::sim::FrameView(&world, &world, 1.0f), &selected, 0,
                                           [&](osc::sim::CommandType) { return &style; });
    REQUIRE(paths.size() == 2);
    REQUIRE(paths[1].legs.size() == 1);
    CHECK(paths[0].chosen);
    CHECK(paths[0].legs[0].line_color == style.line_selected_color);
    CHECK_FALSE(paths[1].chosen);
    CHECK(paths[1].chain.size() == 2);
    CHECK(paths[1].legs[0].line_color == style.line_color);
    const auto nodes = osc::renderer::command_graph_nodes(paths);
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[0].color == style.waypoint_selected_color);
    CHECK(nodes[1].color == style.waypoint_color);
    CHECK(nodes[1].scale == 0.5f);
}

TEST_CASE("Units given one order share its waypoint, at the mean of their targets",
          "[renderer][command_graph]") {
    using osc::sim::CommandType;
    osc::sim::WorldSnapshot world;
    const auto order = [](CommandType type, osc::u32 id, osc::f32 x) {
        osc::sim::CommandRecord c;
        c.type = type;
        c.command_id = id;
        c.target_pos = {x, 0, 50};
        return c;
    };
    const auto add = [&](osc::u32 id, const std::vector<osc::sim::CommandRecord>& orders) {
        osc::sim::EntityRecord e;
        e.id = id;
        e.army = 0;
        e.is_unit = true;
        e.position = {static_cast<osc::f32>(id) * 10.0f, 0, 0};
        e.command_offset = static_cast<osc::u32>(world.commands.size());
        e.command_count = static_cast<osc::u32>(orders.size());
        world.commands.insert(world.commands.end(), orders.begin(), orders.end());
        world.entities.push_back(e);
    };
    add(1, {order(CommandType::Move, 7, 40), order(CommandType::Patrol, 9, 80)});
    add(2, {order(CommandType::Move, 7, 60), order(CommandType::Patrol, 9, 90)});
    add(3, {order(CommandType::Move, 8, 20)});
    add(4, {order(CommandType::Move, 0, 20)});
    add(5, {order(CommandType::Move, 0, 20)});
    osc::renderer::CommandGraphStyle style;
    const std::unordered_set<osc::u32> selected{2};
    const auto nodes = osc::renderer::command_graph_nodes(
        osc::renderer::command_graph_paths(osc::sim::FrameView(&world, &world, 1.0f), &selected, 0,
                                           [&](CommandType) { return &style; }));
    REQUIRE(nodes.size() == 5);
    CHECK(nodes[0].order.command_id == 7);
    CHECK(nodes[0].units == std::vector<osc::u32>{1, 2});
    CHECK(nodes[0].position.x == Catch::Approx(50.0f));
    CHECK(nodes[0].unit_scale == Catch::Approx(1.0f));
    CHECK(nodes[0].chosen);
    CHECK(nodes[1].order.command_id == 9);
    CHECK(nodes[1].position.x == Catch::Approx(85.0f));
    CHECK(nodes[1].unit_scale == Catch::Approx(1.0f));
    CHECK(nodes[2].units == std::vector<osc::u32>{3});
    CHECK_FALSE(nodes[2].chosen);
    CHECK(nodes[3].units == std::vector<osc::u32>{4});
    CHECK(nodes[4].units == std::vector<osc::u32>{5});
}

TEST_CASE("Units given one order together draw one line to it, from their mean",
          "[renderer][command_graph]") {
    using osc::sim::CommandType;
    osc::sim::WorldSnapshot world;
    const auto move = [](osc::u32 id, osc::f32 x) {
        osc::sim::CommandRecord c;
        c.type = CommandType::Move;
        c.command_id = id;
        c.target_pos = {x, 0, 50};
        return c;
    };
    const auto add = [&](osc::u32 id, const std::vector<osc::sim::CommandRecord>& orders) {
        osc::sim::EntityRecord e;
        e.id = id;
        e.army = 0;
        e.is_unit = true;
        e.position = {static_cast<osc::f32>(id) * 10.0f, 0, 0};
        e.command_offset = static_cast<osc::u32>(world.commands.size());
        e.command_count = static_cast<osc::u32>(orders.size());
        world.commands.insert(world.commands.end(), orders.begin(), orders.end());
        world.entities.push_back(e);
    };
    add(1, {move(7, 40), move(9, 80)});
    add(2, {move(7, 60), move(9, 90)});
    add(3, {move(7, 50), move(8, 20)});
    add(4, {move(9, 85)});
    osc::renderer::CommandGraphStyle style;
    style.line_color = {0, 1, 1, 0.2f};
    const auto graph = osc::renderer::command_graph(
        osc::renderer::command_graph_paths(osc::sim::FrameView(&world, &world, 1.0f), nullptr, 0,
                                           [&](CommandType) { return &style; }));
    REQUIRE(graph.edges.size() == 4);
    const auto& start = graph.edges[0];
    CHECK(start.from.x == Catch::Approx(20.0f));
    CHECK(start.to.x == Catch::Approx(50.0f));
    CHECK(start.units == 3);
    CHECK(start.color == style.line_color);
    CHECK(graph.edges[1].from.x == Catch::Approx(50.0f));
    CHECK(graph.edges[1].to.x == Catch::Approx(85.0f));
    CHECK(graph.edges[1].units == 2);
    CHECK(graph.edges[2].to.x == Catch::Approx(20.0f));
    CHECK(graph.edges[2].units == 1);
    CHECK(graph.edges[3].from.x == Catch::Approx(40.0f));
    CHECK(graph.edges[3].to.x == Catch::Approx(85.0f));
    CHECK(graph.edges[3].units == 1);
}

TEST_CASE("A patrol's path runs back to its first patrol point", "[renderer][command_graph]") {
    using osc::sim::CommandType;
    const auto path_of = [](const std::vector<std::pair<CommandType, osc::f32>>& orders) {
        osc::sim::WorldSnapshot world;
        osc::sim::EntityRecord e;
        e.id = 1;
        e.army = 0;
        e.is_unit = true;
        e.command_count = static_cast<osc::u32>(orders.size());
        for (const auto& [type, x] : orders) {
            osc::sim::CommandRecord c;
            c.type = type;
            c.target_pos = {x, 0, 0};
            world.commands.push_back(c);
        }
        world.entities.push_back(e);
        osc::renderer::CommandGraphStyle style;
        const auto paths =
            osc::renderer::command_graph_paths(osc::sim::FrameView(&world, &world, 1.0f), nullptr,
                                               0, [&](CommandType) { return &style; });
        REQUIRE(paths.size() == 1);
        return paths[0];
    };
    const auto loop =
        path_of({{CommandType::Move, 10}, {CommandType::Patrol, 20}, {CommandType::Patrol, 30}});
    REQUIRE(loop.legs.size() == 4);
    CHECK(loop.legs[3].closes);
    CHECK(loop.chain[3].x == 30.0f);
    CHECK(loop.chain[4].x == 20.0f);
    const auto onward =
        path_of({{CommandType::Patrol, 20}, {CommandType::Patrol, 30}, {CommandType::Move, 40}});
    REQUIRE(onward.legs.size() == 4);
    CHECK(onward.chain[4].x == 20.0f);
    CHECK(path_of({{CommandType::Move, 10}, {CommandType::Patrol, 20}}).legs.size() == 2);
    CHECK(path_of({{CommandType::Move, 10}, {CommandType::Move, 20}}).legs.size() == 2);
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

TEST_CASE("The snapshot names an order by the sim's id, one for the units given it together",
          "[renderer][command_graph]") {
    lua_State* L = lua_open();
    {
        osc::sim::SimState sim(L, nullptr);
        std::vector<osc::u32> ids;
        for (int i = 0; i < 2; ++i) {
            auto u = std::make_unique<osc::sim::Unit>();
            u->set_army(0);
            u->set_max_speed(5.0f);
            ids.push_back(sim.entity_registry().register_entity(std::move(u)));
        }
        osc::sim::UnitCommand move;
        move.type = osc::sim::CommandType::Move;
        move.target_pos = {100, 0, 100};
        sim.set_human_input_active(true);
        sim.route_player_command(ids, move, true);
        sim.set_human_input_active(false);
        osc::sim::WorldSnapshot world;
        osc::sim::capture_world(sim, world);
        REQUIRE(world.pending_commands.size() == 2);
        const osc::u32 issued = world.pending_commands[0].command_id;
        CHECK(issued != 0);
        CHECK(world.pending_commands[1].command_id == issued);

        sim.tick();
        osc::sim::capture_world(sim, world);
        REQUIRE(world.commands.size() == 2);
        CHECK(world.commands[0].command_id == issued);
        CHECK(world.commands[0].command_id == world.commands[1].command_id);
    }
    lua_close(L);
}

TEST_CASE("The waypoint under the cursor is the nearest within its size, a selected one first",
          "[renderer][command_graph]") {
    using osc::renderer::waypoint_under_cursor;
    using W = osc::renderer::WaypointOnScreen;
    CHECK(waypoint_under_cursor({W{1, 100, 100, 3, false}}, 105, 100) == 1);
    CHECK(waypoint_under_cursor({W{1, 100, 100, 3, false}}, 120, 100) == 0);
    CHECK(waypoint_under_cursor({W{1, 100, 100, 30, false}}, 120, 100) == 1);
    CHECK(waypoint_under_cursor({W{1, 100, 100, 300, false}}, 250, 100) == 0);
    CHECK(waypoint_under_cursor({W{1, 100, 100, 3, false}, W{2, 104, 100, 3, false}}, 103, 100) ==
          2);
    CHECK(waypoint_under_cursor({W{1, 100, 100, 3, true}, W{2, 104, 100, 3, false}}, 103, 100) ==
          1);
    CHECK(waypoint_under_cursor({W{2, 104, 100, 3, false}, W{1, 100, 100, 3, true}}, 103, 100) ==
          1);
    CHECK(waypoint_under_cursor({}, 100, 100) == 0);
}

TEST_CASE("A waypoint is where the camera shows its node; an unnumbered one is not picked",
          "[renderer][command_graph]") {
    osc::renderer::Camera cam;
    cam.set_viewport(1024.0f, 768.0f);
    cam.init(256.0f, 256.0f);
    osc::renderer::CommandGraphNode node;
    node.order.command_id = 4;
    node.position = {128, 0, 128};
    osc::renderer::CommandGraphNode off = node;
    off.order.command_id = 5;
    off.position = {-5000, 0, 128};
    osc::renderer::CommandGraphNode pending = node;
    pending.order.command_id = 0;
    const auto shown = osc::renderer::waypoints_on_screen({node, off, pending}, cam, 1024, 768);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].command_id == 4);
    CHECK(shown[0].x == Catch::Approx(512.0f).margin(2.0f));
    CHECK(shown[0].y == Catch::Approx(384.0f).margin(2.0f));
    CHECK(osc::renderer::waypoint_under_cursor(shown, 514, 386) == 4);
}

TEST_CASE("The order under the cursor, and a hovered unit's, are drawn highlighted",
          "[renderer][command_graph]") {
    osc::sim::WorldSnapshot world;
    for (osc::u32 id : {1u, 2u}) {
        osc::sim::EntityRecord e;
        e.id = id;
        e.army = 0;
        e.is_unit = true;
        e.command_offset = static_cast<osc::u32>(world.commands.size());
        e.command_count = 1;
        osc::sim::CommandRecord move;
        move.type = osc::sim::CommandType::Move;
        move.command_id = 10 + id;
        world.commands.push_back(move);
        world.entities.push_back(e);
    }
    osc::renderer::CommandGraphStyle style;
    style.waypoint_color = {1, 1, 1, 0.25f};
    style.waypoint_highlight_color = {1, 1, 1, 1};
    style.waypoint_highlight_scale = 2.0f;
    const auto paths =
        osc::renderer::command_graph_paths(osc::sim::FrameView(&world, &world, 1.0f), nullptr, 0,
                                           [&](osc::sim::CommandType) { return &style; });
    auto nodes = osc::renderer::command_graph_nodes(paths, 12, 0);
    REQUIRE(nodes.size() == 2);
    CHECK_FALSE(nodes[0].highlighted);
    CHECK(nodes[0].color == style.waypoint_color);
    CHECK(nodes[1].highlighted);
    CHECK(nodes[1].color == style.waypoint_highlight_color);
    CHECK(nodes[1].scale == 2.0f);
    nodes = osc::renderer::command_graph_nodes(paths, 0, 1);
    CHECK(nodes[0].highlighted);
    CHECK_FALSE(nodes[1].highlighted);
}
