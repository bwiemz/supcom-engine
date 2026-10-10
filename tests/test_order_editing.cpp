#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/command_graph_renderer.hpp"
#include "renderer/input_handler.hpp"
#include "sim/formation.hpp"
#include "sim/manipulator.hpp"
#include "sim/replay.hpp"
#include "sim/sim_callback_queue.hpp"
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
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SimCallbackEntry;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct World {
    lua_State* L = lua_open();
    std::unique_ptr<SimState> sim = std::make_unique<SimState>(L, nullptr);

    World() {
        std::vector<osc::u16> heights(129 * 129, 1000);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim->set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim->build_pathfinding_grid();
        sim->add_army("ARMY_1", "ARMY_1");
        sim->add_army("ARMY_2", "ARMY_2");
        sim->set_fog_of_war("none");
        sim->set_victory_condition("sandbox");
    }
    ~World() {
        sim.reset();
        lua_close(L);
    }

    Unit& walker(osc::f32 x, osc::f32 z, int army = 0) {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
        u->set_max_speed(5.0f);
        u->set_position({x, 0.0f, z});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        Unit::Drive drive;
        drive.max_accel = 50.0f;
        u->set_drive(drive);
        u->add_command_cap("RULEUCC_Patrol");
        auto* raw = u.get();
        sim->entity_registry().register_entity(std::move(u));
        return *raw;
    }

    void order(const std::vector<osc::u32>& ids, CommandType type, osc::f32 x, osc::f32 z,
               bool clear = true, osc::u32 target = 0) {
        UnitCommand c;
        c.type = type;
        c.target_pos = {x, 0.0f, z};
        c.target_id = target;
        sim->set_human_input_active(true);
        sim->route_player_command(ids, c, clear);
        sim->set_human_input_active(false);
    }

    void ticks(int n) {
        for (int i = 0; i < n; ++i) {
            sim->tick();
        }
    }
};

SimCallbackEntry retarget(osc::u32 command, std::vector<osc::u32> units, osc::f32 x, osc::f32 z) {
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kSetCommandTargetCallback;
    cb.args["Command"] = static_cast<double>(command);
    cb.args["X"] = static_cast<double>(x);
    cb.args["Y"] = 0.0;
    cb.args["Z"] = static_cast<double>(z);
    cb.unit_ids = std::move(units);
    return cb;
}

osc::f32 distance(const Unit& u, osc::f32 x, osc::f32 z) {
    return std::hypot(u.position().x - x, u.position().z - z);
}

} // namespace

TEST_CASE("Moving an order's target sends every unit given it there", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    w.order({a.entity_id(), b.entity_id()}, CommandType::Move, 100, 15);
    w.ticks(2);
    REQUIRE(a.command_queue().size() == 1);
    const osc::u32 id = a.command_queue().front().command_id;
    REQUIRE(b.command_queue().front().command_id == id);

    w.sim->submit_callback(retarget(id, {a.entity_id(), b.entity_id()}, 30, 90));
    CHECK(a.command_queue().front().target_pos.x == 100.0f);
    w.ticks(1);
    CHECK(a.command_queue().front().target_pos.x == 30.0f);
    CHECK(b.command_queue().front().target_pos.z == 90.0f);
    w.ticks(300);
    CHECK(distance(a, 30, 90) < 3.0f);
    CHECK(distance(b, 30, 90) < 3.0f);
}

TEST_CASE("Another army's player can't move an order", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    w.order({a.entity_id()}, CommandType::Move, 100, 15);
    w.ticks(2);
    const osc::u32 id = a.command_queue().front().command_id;
    w.sim->set_source_army(1, 1);
    w.sim->schedule_callback(1, retarget(id, {a.entity_id()}, 30, 90));
    w.ticks(2);
    CHECK(a.command_queue().front().target_pos.x == 100.0f);
}

TEST_CASE("A moved order's target is kept to the playable area", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    w.sim->set_playable_rect(0, 0, 64, 64);
    w.order({a.entity_id()}, CommandType::Move, 50, 50);
    w.ticks(2);
    w.sim->submit_callback(retarget(a.command_queue().front().command_id, {a.entity_id()}, 90, 20));
    w.ticks(1);
    CHECK(a.command_queue().front().target_pos.x == 64.0f);
    CHECK(a.command_queue().front().target_pos.z == 20.0f);
}

TEST_CASE("A replay moves the order as the game did", "[order_edit][replay]") {
    osc::sim::Replay replay;
    osc::sim::Vector3 played;
    {
        World w;
        Unit& a = w.walker(10, 10);
        w.sim->set_recording(true);
        w.order({a.entity_id()}, CommandType::Move, 100, 15);
        w.ticks(2);
        w.sim->submit_callback(
            retarget(a.command_queue().front().command_id, {a.entity_id()}, 30, 90));
        w.ticks(100);
        played = a.position();
        REQUIRE(osc::sim::Replay::deserialize(w.sim->recorded_replay().serialize(), replay));
    }
    World w;
    Unit& a = w.walker(10, 10);
    w.sim->queue_replay(replay);
    w.ticks(102);
    CHECK(a.position().x == played.x);
    CHECK(a.position().z == played.z);
    REQUIRE_FALSE(a.command_queue().empty());
    CHECK(a.command_queue().front().target_pos.z == 90.0f);
}

TEST_CASE("A moved patrol point stays moved, lap after lap", "[order_edit][patrol]") {
    World w;
    Unit& a = w.walker(10, 10);
    w.order({a.entity_id()}, CommandType::Patrol, 60, 10);
    w.order({a.entity_id()}, CommandType::Patrol, 60, 60, false);
    w.ticks(2);
    REQUIRE(a.command_queue().size() == 2);
    const osc::u32 second = a.command_queue()[1].command_id;
    w.sim->submit_callback(retarget(second, {a.entity_id()}, 10, 60));
    int visits = 0;
    bool near = false;
    bool old_point = false;
    for (int t = 0; t < 1200; ++t) {
        w.sim->tick();
        const bool now = distance(a, 10, 60) < 3.0f;
        visits += now && !near ? 1 : 0;
        near = now;
        old_point = old_point || distance(a, 60, 60) < 3.0f;
    }
    CHECK(visits >= 2);
    CHECK_FALSE(old_point);
    REQUIRE(a.command_queue().size() == 2);
    for (const UnitCommand& c : a.command_queue()) {
        if (c.command_id == second) {
            CHECK(c.target_pos.x == 10.0f);
            CHECK(c.target_pos.z == 60.0f);
        }
    }
}

TEST_CASE("An order on a unit moves to another unit of the same army", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    a.add_command_cap("RULEUCC_Guard");
    Unit& friend1 = w.walker(40, 10);
    Unit& friend2 = w.walker(40, 40);
    Unit& enemy = w.walker(60, 60, 1);
    w.order({a.entity_id()}, CommandType::Guard, 40, 10, true, friend1.entity_id());
    w.ticks(2);
    REQUIRE(a.command_queue().front().target_id == friend1.entity_id());
    const osc::u32 id = a.command_queue().front().command_id;

    SimCallbackEntry cb = retarget(id, {a.entity_id()}, 0, 0);
    cb.args["Target"] = static_cast<double>(enemy.entity_id());
    w.sim->submit_callback(cb);
    w.ticks(1);
    CHECK(a.command_queue().front().target_id == friend1.entity_id());

    cb.args["Target"] = static_cast<double>(friend2.entity_id());
    w.sim->submit_callback(cb);
    w.ticks(1);
    CHECK(a.command_queue().front().target_id == friend2.entity_id());
}

TEST_CASE("A moved formation order keeps its shape", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(20, 10);
    UnitCommand c;
    c.type = CommandType::Move;
    c.command_id = 77;
    c.formation = "GrowthFormation";
    c.target_pos = {50, 0, 50};
    a.push_command(c, true);
    c.target_pos = {54, 0, 50};
    b.push_command(c, true);
    w.sim->run_sim_callback(retarget(77, {a.entity_id(), b.entity_id()}, 82, 30));
    CHECK(a.command_queue().front().target_pos.x == 80.0f);
    CHECK(b.command_queue().front().target_pos.x == 84.0f);
    CHECK(b.command_queue().front().target_pos.z == 30.0f);
}

TEST_CASE("A moved build order snaps to its footprint, unless it is under way", "[order_edit]") {
    World w;
    const char* code = "__blueprints = { ueb0101 = { Footprint = { SizeX = 2, SizeZ = 2 } } }";
    REQUIRE(luaL_loadbuffer(w.L, code, std::strlen(code), "bp") == 0);
    REQUIRE(lua_pcall(w.L, 0, 0, 0) == 0);
    Unit& a = w.walker(10, 10);
    UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.blueprint_id = "ueb0101";
    build.command_id = 5;
    build.target_pos = {60, 0, 60};
    UnitCommand stop;
    stop.type = CommandType::Move;
    stop.target_pos = {20, 0, 20};
    a.push_command(stop, true);
    a.push_command(build, false);
    w.sim->run_sim_callback(retarget(5, {a.entity_id()}, 30.3f, 40.6f));
    CHECK(a.command_queue()[1].target_pos.x == 30.0f);
    CHECK(a.command_queue()[1].target_pos.z == 41.0f);

    a.push_command(build, true);
    a.set_build_target_id(99);
    w.sim->run_sim_callback(retarget(5, {a.entity_id()}, 30, 40));
    CHECK(a.command_queue().front().target_pos.x == 60.0f);
}

TEST_CASE("An order with no place, or of no unit named, is left alone", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    UnitCommand stop;
    stop.type = CommandType::Upgrade;
    stop.command_id = 3;
    a.push_command(stop, true);
    UnitCommand move;
    move.type = CommandType::Move;
    move.command_id = 4;
    move.target_pos = {50, 0, 50};
    b.push_command(move, true);
    w.sim->run_sim_callback(retarget(3, {a.entity_id()}, 30, 30));
    CHECK(a.command_queue().front().target_pos.x == 0.0f);
    w.sim->run_sim_callback(retarget(4, {a.entity_id()}, 30, 30));
    CHECK(b.command_queue().front().target_pos.x == 50.0f);
}

namespace {

SimCallbackEntry removal(osc::u32 command, std::vector<osc::u32> units) {
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kRemoveCommandCallback;
    cb.args["Command"] = static_cast<double>(command);
    cb.unit_ids = std::move(units);
    return cb;
}

std::vector<osc::u32> ids_of(const Unit& u) {
    std::vector<osc::u32> ids;
    for (const UnitCommand& c : u.command_queue()) {
        ids.push_back(c.command_id);
    }
    return ids;
}

} // namespace

TEST_CASE("Taking an order off a queue leaves the orders either side", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    const std::vector<osc::u32> both = {a.entity_id(), b.entity_id()};
    w.order(both, CommandType::Move, 60, 10);
    w.order(both, CommandType::Move, 60, 60, false);
    w.order(both, CommandType::Move, 10, 60, false);
    w.ticks(2);
    const auto before = ids_of(a);
    REQUIRE(before.size() == 3);
    w.sim->submit_callback(removal(before[1], both));
    w.ticks(1);
    CHECK(ids_of(a) == std::vector<osc::u32>{before[0], before[2]});
    CHECK(ids_of(b) == std::vector<osc::u32>{before[0], before[2]});
}

TEST_CASE("Taking off the order under way sends the unit on to the next", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    w.order({a.entity_id()}, CommandType::Move, 100, 10);
    w.order({a.entity_id()}, CommandType::Move, 10, 60, false);
    w.ticks(10);
    const auto before = ids_of(a);
    REQUIRE(before.size() == 2);
    w.sim->submit_callback(removal(before[0], {a.entity_id()}));
    w.ticks(300);
    CHECK(distance(a, 10, 60) < 3.0f);
    CHECK(a.position().x < 40.0f);
}

TEST_CASE("Another army's player can't take an order off", "[order_edit]") {
    World w;
    Unit& a = w.walker(10, 10);
    w.order({a.entity_id()}, CommandType::Move, 100, 10);
    w.ticks(2);
    const auto before = ids_of(a);
    w.sim->set_source_army(1, 1);
    w.sim->schedule_callback(1, removal(before[0], {a.entity_id()}));
    w.ticks(2);
    CHECK(ids_of(a) == before);
}

TEST_CASE("A factory's rally order can be taken off", "[order_edit]") {
    World w;
    Unit& f = w.walker(10, 10);
    UnitCommand rally;
    rally.type = CommandType::Move;
    rally.command_id = 8;
    f.add_rally_order(rally);
    rally.command_id = 9;
    f.add_rally_order(rally);
    w.sim->run_sim_callback(removal(8, {f.entity_id()}));
    REQUIRE(f.rally_orders().size() == 1);
    CHECK(f.rally_orders()[0].command_id == 9);
}

namespace {

struct Ui {
    osc::sim::WorldSnapshot world;
    osc::renderer::InputHandler input;
    std::vector<SimCallbackEntry> sent;

    explicit Ui(SimState& sim) {
        osc::sim::capture_world(sim, world);
        input.set_frame_view(osc::sim::FrameView(&world, &world, 1.0f));
        input.set_player_army(0);
        sim.set_local_callback_sink([this](SimCallbackEntry cb) { sent.push_back(std::move(cb)); });
    }
};

double number(const SimCallbackEntry& cb, const char* key) {
    return std::get<double>(cb.args.at(key));
}

} // namespace

TEST_CASE("Dragging a waypoint sends one SetCommandTarget, on release", "[order_edit][input]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    w.order({a.entity_id(), b.entity_id()}, CommandType::Move, 100, 15);
    w.ticks(2);
    const osc::u32 id = a.command_queue().front().command_id;
    Ui ui(*w.sim);
    REQUIRE(ui.input.begin_order_drag(id));
    for (osc::f32 x : {90.0f, 70.0f, 50.0f}) {
        ui.input.drag_order_to(*w.sim, x, 60);
        CHECK(ui.sent.empty());
        REQUIRE(ui.input.order_drag());
        CHECK(ui.input.order_drag()->at.x == x);
        CHECK(ui.input.order_drag()->held);
    }
    const auto sent = ui.input.release_order_drag(*w.sim, 30, 90);
    REQUIRE(ui.sent.size() == 1);
    CHECK(ui.sent[0].func_name == osc::sim::kSetCommandTargetCallback);
    CHECK(number(ui.sent[0], "Command") == id);
    CHECK(number(ui.sent[0], "X") == 30.0);
    CHECK(number(ui.sent[0], "Z") == 90.0);
    CHECK(ui.sent[0].unit_ids == std::vector<osc::u32>{a.entity_id(), b.entity_id()});
    REQUIRE(ui.input.order_drag());
    CHECK_FALSE(ui.input.order_drag()->held);
    CHECK_FALSE(ui.input.release_order_drag(*w.sim, 30, 90));
    CHECK(ui.sent.size() == 1);
}

TEST_CASE("A waypoint pressed and let go where it was sends nothing", "[order_edit][input]") {
    World w;
    Unit& a = w.walker(10, 10);
    w.order({a.entity_id()}, CommandType::Move, 100, 15);
    w.ticks(2);
    Ui ui(*w.sim);
    REQUIRE(ui.input.begin_order_drag(a.command_queue().front().command_id));
    CHECK_FALSE(ui.input.release_order_drag(*w.sim, 100, 15));
    CHECK(ui.sent.empty());
    CHECK_FALSE(ui.input.begin_order_drag(0));
    ui.input.set_player_army(-1);
    CHECK_FALSE(ui.input.begin_order_drag(a.command_queue().front().command_id));
}

TEST_CASE("An order on a unit is dropped on the nearest unit of its target's army",
          "[order_edit][input]") {
    World w;
    Unit& a = w.walker(10, 10);
    a.add_command_cap("RULEUCC_Guard");
    Unit& friend1 = w.walker(40, 10);
    Unit& friend2 = w.walker(40, 60);
    w.walker(50, 50, 1);
    w.order({a.entity_id()}, CommandType::Guard, 40, 10, true, friend1.entity_id());
    w.ticks(2);
    Ui ui(*w.sim);
    REQUIRE(ui.input.begin_order_drag(a.command_queue().front().command_id));
    ui.input.drag_order_to(*w.sim, 50, 52);
    CHECK(ui.input.order_drag()->at.x == 50.0f);
    REQUIRE(ui.input.release_order_drag(*w.sim, 50, 52));
    REQUIRE(ui.sent.size() == 1);
    CHECK(number(ui.sent[0], "Target") == friend2.entity_id());
    CHECK(ui.sent[0].args.count("X") == 0);
}

TEST_CASE("A build dropped where it can't stand is not sent; elsewhere it snaps",
          "[order_edit][input]") {
    World w;
    Unit& a = w.walker(10, 10);
    UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.blueprint_id = "pgen";
    build.target_pos = {30, 0, 30};
    w.sim->set_human_input_active(true);
    w.sim->route_player_command({a.entity_id()}, build, true);
    w.sim->set_human_input_active(false);
    w.ticks(1);
    const osc::u32 id = a.command_queue().front().command_id;
    Ui ui(*w.sim);
    osc::u32 moving = 0;
    osc::renderer::CommandModeHooks hooks;
    hooks.can_place = [&](osc::i32, const std::string& bp, osc::f32 x, osc::f32, osc::u32 m) {
        osc::sim::PlacementRules r;
        r.size_x = r.size_z = 2.0f;
        w.sim->placement_rules(bp, [&] { return r; });
        moving = m;
        return x < 50.0f;
    };
    ui.input.set_command_mode_hooks(std::move(hooks));
    REQUIRE(ui.input.begin_order_drag(id));
    ui.input.drag_order_to(*w.sim, 60.4f, 30);
    CHECK_FALSE(ui.input.order_drag()->valid);
    CHECK_FALSE(ui.input.release_order_drag(*w.sim, 60.4f, 30));
    CHECK(ui.sent.empty());

    REQUIRE(ui.input.begin_order_drag(id));
    ui.input.drag_order_to(*w.sim, 40.4f, 30.6f);
    CHECK(ui.input.order_drag()->valid);
    REQUIRE(ui.input.release_order_drag(*w.sim, 40.4f, 30.6f));
    CHECK(moving == id);
    REQUIRE(ui.sent.size() == 1);
    CHECK(number(ui.sent[0], "X") == 40.0);
    CHECK(number(ui.sent[0], "Z") == 31.0);
}

TEST_CASE("Shift+Ctrl and the right button over a waypoint take its order off",
          "[order_edit][input]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    const std::vector<osc::u32> both = {a.entity_id(), b.entity_id()};
    w.order(both, CommandType::Move, 60, 10);
    w.order(both, CommandType::Move, 60, 60, false);
    w.ticks(2);
    const osc::u32 second = a.command_queue()[1].command_id;
    osc::sim::WorldSnapshot world;
    osc::sim::capture_world(*w.sim, world);
    osc::renderer::InputHandler input;
    input.set_frame_view(osc::sim::FrameView(&world, &world, 1.0f));
    input.set_player_army(0);
    CHECK(input.removes_order(second, true, true));
    CHECK_FALSE(input.removes_order(second, true, false));
    CHECK_FALSE(input.removes_order(second, false, true));
    CHECK_FALSE(input.removes_order(0, true, true));
    REQUIRE(input.remove_order(*w.sim, second));
    w.ticks(1);
    CHECK(a.command_queue().size() == 1);
    CHECK(b.command_queue().size() == 1);
    input.set_player_army(-1);
    CHECK_FALSE(input.removes_order(a.command_queue()[0].command_id, true, true));
    CHECK_FALSE(input.remove_order(*w.sim, a.command_queue()[0].command_id));
}

TEST_CASE("Another army's orders shown on the graph can't be moved or taken off",
          "[order_edit][input]") {
    World w;
    Unit& theirs = w.walker(10, 10, 1);
    w.order({theirs.entity_id()}, CommandType::Move, 60, 10);
    w.ticks(2);
    const osc::u32 id = theirs.command_queue().front().command_id;
    Ui ui(*w.sim);
    ui.input.set_selected({theirs.entity_id()});
    CHECK_FALSE(ui.input.remove_order(*w.sim, id));
    REQUIRE(ui.input.begin_order_drag(id));
    ui.input.drag_order_to(*w.sim, 30, 30);
    CHECK_FALSE(ui.input.release_order_drag(*w.sim, 30, 30));
    CHECK(ui.sent.empty());
}

TEST_CASE("A dragged waypoint draws its order's legs to where it is", "[order_edit][renderer]") {
    osc::sim::WorldSnapshot world;
    for (osc::u32 id : {1u, 2u}) {
        osc::sim::EntityRecord e;
        e.id = id;
        e.army = 0;
        e.is_unit = true;
        e.command_offset = static_cast<osc::u32>(world.commands.size());
        e.command_count = 1;
        osc::sim::CommandRecord move;
        move.type = CommandType::Move;
        move.command_id = 7;
        move.target_pos = {static_cast<osc::f32>(40 + 20 * (id - 1)), 0, 50};
        world.commands.push_back(move);
        world.entities.push_back(e);
    }
    osc::renderer::CommandGraphStyle style;
    auto paths = osc::renderer::command_graph_paths(
        osc::sim::FrameView(&world, &world, 1.0f), nullptr, 0, [&](CommandType) { return &style; });
    osc::renderer::preview_paths(paths, 7, {80, 0, 20});
    CHECK(paths[0].chain[1].x == 70.0f);
    CHECK(paths[1].chain[1].x == 90.0f);
    CHECK(paths[1].chain[1].z == 20.0f);
    CHECK(osc::renderer::command_graph_nodes(paths)[0].position.x == 80.0f);
}

TEST_CASE("A click on an earlier move's waypoint makes the moves from it a patrol",
          "[order_edit][input][patrol]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    const std::vector<osc::u32> both = {a.entity_id(), b.entity_id()};
    w.order(both, CommandType::Move, 60, 10);
    w.order(both, CommandType::Move, 60, 60, false);
    w.order(both, CommandType::Move, 10, 60, false);
    w.ticks(2);
    const auto ids = ids_of(a);
    REQUIRE(ids.size() == 3);
    Ui ui(*w.sim);
    ui.input.set_selected({a.entity_id(), b.entity_id()});
    CHECK(ui.input.moves_to_patrol(*w.sim, ids[2]).empty());
    REQUIRE(ui.input.moves_to_patrol(*w.sim, ids[1]) == std::vector<osc::u32>{ids[1], ids[2]});
    ui.input.restart_as_patrol(*w.sim, ids[1]);
    REQUIRE(ui.sent.size() == 2);
    CHECK(ui.sent[0].func_name == osc::sim::kSetCommandTypeCallback);
    CHECK(number(ui.sent[0], "Command") == ids[1]);
    CHECK(ui.sent[0].unit_ids == both);
    w.sim->set_local_callback_sink(nullptr);
    for (const SimCallbackEntry& cb : ui.sent) {
        w.sim->submit_callback(cb);
    }
    w.ticks(1);
    for (const Unit* u : {&a, &b}) {
        REQUIRE(u->command_queue().size() == 3);
        CHECK(u->command_queue()[0].type == CommandType::Move);
        CHECK(u->command_queue()[1].type == CommandType::Patrol);
        CHECK(u->command_queue()[2].type == CommandType::Patrol);
    }
    w.ticks(600);
    CHECK(a.command_queue().size() == 2);
    CHECK(a.command_queue().front().type == CommandType::Patrol);
}

TEST_CASE("A queue with more than moves, or not holding the waypoint, starts no patrol",
          "[order_edit][input][patrol]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(10, 20);
    w.order({a.entity_id(), b.entity_id()}, CommandType::Move, 60, 10);
    w.order({a.entity_id()}, CommandType::Move, 60, 60, false);
    w.order({b.entity_id()}, CommandType::Patrol, 60, 60, false);
    w.ticks(2);
    const osc::u32 first = a.command_queue()[0].command_id;
    Ui ui(*w.sim);
    ui.input.set_selected({a.entity_id()});
    CHECK(ui.input.moves_to_patrol(*w.sim, first).size() == 2);
    ui.input.set_selected({a.entity_id(), b.entity_id()});
    CHECK(ui.input.moves_to_patrol(*w.sim, first).empty());
    ui.input.set_selected({a.entity_id()});
    CHECK(ui.input.moves_to_patrol(*w.sim, b.command_queue()[1].command_id).empty());
    CHECK(ui.input.restart_as_patrol(*w.sim, b.command_queue()[1].command_id).empty());
    CHECK(ui.sent.empty());
}

TEST_CASE("Only a move is made a patrol, and only of the sender's units", "[order_edit][patrol]") {
    World w;
    Unit& a = w.walker(10, 10);
    a.add_weapon(std::make_unique<osc::sim::Weapon>());
    Unit& theirs = w.walker(10, 20, 1);
    w.order({a.entity_id()}, CommandType::Move, 60, 10);
    w.order({a.entity_id()}, CommandType::Attack, 60, 60, false);
    w.order({theirs.entity_id()}, CommandType::Move, 60, 60);
    w.ticks(2);
    const auto patrol = [](osc::u32 command, osc::u32 unit, CommandType type) {
        SimCallbackEntry cb;
        cb.func_name = osc::sim::kSetCommandTypeCallback;
        cb.args["Command"] = static_cast<double>(command);
        cb.args["Type"] = static_cast<double>(type);
        cb.unit_ids = {unit};
        return cb;
    };
    w.sim->set_source_army(1, 0);
    w.sim->schedule_callback(
        1, patrol(a.command_queue()[1].command_id, a.entity_id(), CommandType::Patrol));
    w.sim->schedule_callback(
        1, patrol(a.command_queue()[0].command_id, a.entity_id(), CommandType::Attack));
    w.sim->schedule_callback(
        1, patrol(theirs.command_queue()[0].command_id, theirs.entity_id(), CommandType::Patrol));
    w.ticks(2);
    CHECK(a.command_queue()[0].type == CommandType::Move);
    CHECK(a.command_queue()[1].type == CommandType::Attack);
    CHECK(theirs.command_queue()[0].type == CommandType::Move);
}

namespace {

osc::renderer::InputHandler formation_input(const std::vector<osc::u32>& selected) {
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({selected.begin(), selected.end()});
    osc::renderer::CommandModeHooks hooks;
    hooks.formation_scripts = [](bool) {
        return std::vector<std::string>{"AttackFormation", "GrowthFormation"};
    };
    input.set_command_mode_hooks(hooks);
    return input;
}

UnitCommand pending_order(const SimState& sim, osc::u32 unit) {
    const auto queues = sim.queues_with_pending();
    const auto it = queues.find(unit);
    REQUIRE(it != queues.end());
    REQUIRE_FALSE(it->second.orders.empty());
    return it->second.orders.back();
}

} // namespace

TEST_CASE("A right button held moves the selection in formation, facing the drag",
          "[order_edit][input][formation]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(14, 10);
    auto input = formation_input({a.entity_id(), b.entity_id()});
    input.right_press(*w.sim, 60, 60, false);
    CHECK(w.sim->queues_with_pending().empty());
    input.right_drag(std::array<osc::f32, 2>{70, 60}, 0.3);
    input.right_drag(std::array<osc::f32, 2>{70, 60}, 0.3);
    REQUIRE(input.formation_drag());
    CHECK(input.formation_drag()->settled());
    input.right_release(*w.sim);
    for (const Unit* u : {&a, &b}) {
        const UnitCommand c = pending_order(*w.sim, u->entity_id());
        CHECK(c.type == CommandType::Move);
        CHECK(c.formation == "AttackFormation");
        CHECK(c.has_facing);
        CHECK(std::abs(c.facing - 3.14159265f / 2) < 1e-4f);
        CHECK(c.target_pos.x == 60.0f);
        CHECK(c.target_pos.z == 60.0f);
    }
    CHECK_FALSE(input.formation_drag());
}

TEST_CASE("A right click let go before the formation settles is a plain move",
          "[order_edit][input][formation]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(14, 10);
    auto input = formation_input({a.entity_id(), b.entity_id()});
    input.right_press(*w.sim, 60, 60, false);
    input.right_drag(std::array<osc::f32, 2>{70, 60}, 0.3);
    input.right_release(*w.sim);
    const UnitCommand c = pending_order(*w.sim, a.entity_id());
    CHECK(c.type == CommandType::Move);
    CHECK(c.formation.empty());
    CHECK_FALSE(c.has_facing);
}

TEST_CASE("A left press in a drag formation takes the next script, from then on",
          "[order_edit][input][formation]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(14, 10);
    auto input = formation_input({a.entity_id(), b.entity_id()});
    input.right_press(*w.sim, 12, 60, false);
    REQUIRE(input.cycle_formation());
    input.right_release(*w.sim);
    const UnitCommand c = pending_order(*w.sim, a.entity_id());
    CHECK(c.formation == "GrowthFormation");
    CHECK(c.has_facing);
    CHECK(std::abs(c.facing) < 1e-4f);
    input.right_press(*w.sim, 12, 60, false);
    REQUIRE(input.formation_drag());
    CHECK(input.formation_drag()->script == "GrowthFormation");
}

TEST_CASE("A drag formation's scripts are formations.lua's air or surface ones", "[formation]") {
    lua_State* L = lua_open();
    const std::string chunk = "function import(path) return { SurfaceFormations = { 'A', 'B' }, "
                              "AirFormations = { 'C' } } end";
    REQUIRE(luaL_loadbuffer(L, chunk.data(), chunk.size(), "formations") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    CHECK(osc::sim::formation_scripts(L, false) == std::vector<std::string>{"A", "B"});
    CHECK(osc::sim::formation_scripts(L, true) == std::vector<std::string>{"C"});
    CHECK(lua_gettop(L) == 0);
    lua_close(L);
}

TEST_CASE("A settled drag formation shows each unit's ghost in its slot, facing the drag",
          "[order_edit][input][formation]") {
    World w;
    Unit& a = w.walker(10, 10);
    Unit& b = w.walker(14, 10);
    lua_State* L = lua_open();
    const std::string chunk =
        "function import(path) return { Line = function(units) return { { -1, 0 }, { 1, 0 } } "
        "end } end";
    REQUIRE(luaL_loadbuffer(L, chunk.data(), chunk.size(), "formations") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({a.entity_id(), b.entity_id()});
    osc::renderer::CommandModeHooks hooks;
    hooks.formation_scripts = [](bool) { return std::vector<std::string>{"Line"}; };
    hooks.formation_slots = [&](const std::vector<osc::sim::FormationMember>& units,
                                const std::string& script, const osc::sim::Vector3& at,
                                osc::f32 facing) {
        return osc::sim::plan_formation(
            L, units,
            [](lua_State* s, const osc::sim::FormationMember& u) { lua_pushnumber(s, u.id); },
            nullptr, script, at, facing);
    };
    input.set_command_mode_hooks(hooks);
    const auto sorted = [&](bool xs) {
        std::vector<osc::f32> v;
        for (const auto& g : input.formation_ghosts(*w.sim)) {
            v.push_back(std::round(xs ? g.position.x : g.position.z));
        }
        std::sort(v.begin(), v.end());
        return v;
    };

    input.right_press(*w.sim, 60, 60, false);
    input.right_drag(std::array<osc::f32, 2>{60, 70}, 0.3);
    CHECK(input.formation_ghosts(*w.sim).empty());
    input.right_drag(std::array<osc::f32, 2>{70, 60}, 0.3);
    REQUIRE(input.formation_ghosts(*w.sim).size() == 2);
    CHECK(std::abs(input.formation_ghosts(*w.sim)[0].heading - 3.14159265f / 2) < 1e-4f);
    CHECK(sorted(true) == std::vector<osc::f32>{60, 60});
    CHECK(sorted(false) == std::vector<osc::f32>{57, 63});
    input.right_drag(std::array<osc::f32, 2>{60, 50}, 0.1);
    CHECK(sorted(true) == std::vector<osc::f32>{57, 63});
    CHECK(sorted(false) == std::vector<osc::f32>{60, 60});
    input.right_release(*w.sim);
    CHECK(input.formation_ghosts(*w.sim).empty());
    lua_close(L);
}
