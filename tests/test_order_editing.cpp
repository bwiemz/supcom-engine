#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/replay.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <cstring>
#include <memory>
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
