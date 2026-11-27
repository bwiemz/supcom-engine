#include <catch2/catch_test_macros.hpp>

#include "sim/id_pool.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>

using osc::sim::CommandType;
using osc::sim::EntityId;
using osc::sim::IdPool;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

struct World {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    World() {
        sim.set_victory_condition("sandbox");
        sim.add_army("ARMY_1", "Player");
    }
    Unit& walker(osc::f32 x) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_max_speed(1.0f);
        u->set_position({x, 0.0f, 0.0f});
        return unit(sim.entity_registry().register_entity(std::move(u)));
    }
    Unit& factory(osc::f32 x) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->add_category("FACTORY");
        u->set_position({x, 0.0f, 0.0f});
        Unit& made = unit(sim.entity_registry().register_entity(std::move(u)));
        made.set_up_initial_rally(g.L, &sim);
        return made;
    }
    Unit& unit(EntityId id) { return *static_cast<Unit*>(sim.entity_registry().find(id)); }
    osc::u32 move(Unit& u) {
        UnitCommand c;
        c.type = CommandType::Move;
        c.target_pos = {u.position().x, 0.0f, 1000.0f};
        return sim.route_command({u.entity_id()}, c, true);
    }
    void ticks(int n) {
        for (int i = 0; i < n; ++i) {
            sim.tick();
        }
    }
};

} // namespace

TEST_CASE("Moho's IdPool hands out its least free serial, one given back 99 updates later",
          "[command_ids]") {
    IdPool pool;
    CHECK(pool.take() == 0);
    CHECK(pool.take() == 1);
    CHECK(pool.take() == 2);
    CHECK(pool.take() == 3);
    pool.give_back(1);
    for (size_t i = 1; i < IdPool::kHeldUpdates; ++i) {
        pool.update();
    }
    CHECK(pool.take() == 4);
    pool.update();
    CHECK(pool.take() == 1);
    CHECK(pool.take() == 5);
}

TEST_CASE("Moho's IdPool takes its cursor back over the top serials given back", "[command_ids]") {
    IdPool pool;
    for (int i = 0; i < 4; ++i) {
        pool.take();
    }
    pool.give_back(3);
    pool.give_back(2);
    pool.give_back(0);
    for (size_t i = 0; i < IdPool::kHeldUpdates; ++i) {
        pool.update();
    }
    CHECK(pool.next == 2);
    CHECK(pool.released == std::set<osc::u32>{0});
    CHECK(pool.take() == 0);
    CHECK(pool.take() == 2);
}

TEST_CASE("An order the sim issues is numbered 0x80000000 | a serial, taken as a unit takes it",
          "[command_ids]") {
    World w;
    Unit& a = w.walker(0.0f);
    Unit& b = w.walker(10.0f);
    CHECK(w.move(a) == 0x80000000u);
    UnitCommand nobody;
    nobody.type = CommandType::Move;
    CHECK(w.sim.route_command({}, nobody, true) == 0);
    CHECK(w.move(b) == 0x80000001u);
    REQUIRE(a.command_queue().size() == 1);
    CHECK(a.command_queue().front().command_id == 0x80000000u);
    CHECK(a.command_queue().front().serial < b.command_queue().front().serial);
}

TEST_CASE("The sim's order no unit holds gives its serial back, free again 99 beats on",
          "[command_ids]") {
    World w;
    Unit& a = w.walker(0.0f);
    REQUIRE(w.move(a) == 0x80000000u);
    a.clear_commands("");
    w.ticks(98);
    Unit& b = w.walker(10.0f);
    CHECK(w.move(b) == 0x80000001u);
    w.ticks(1);
    Unit& c = w.walker(20.0f);
    CHECK(w.move(c) == 0x80000000u);
}

TEST_CASE("A factory's initial rally is the sim's order, set up again once its rally is cleared",
          "[command_ids]") {
    World w;
    Unit& f = w.factory(0.0f);
    REQUIRE(f.rally_orders().size() == 1);
    CHECK(f.rally_orders().front().command_id == 0x80000000u);
    f.clear_rally_orders();
    w.ticks(1);
    REQUIRE(f.rally_orders().size() == 1);
    CHECK(f.rally_orders().front().command_id == 0x80000001u);
}

TEST_CASE(
    "A factory's rally order to board is dropped at its motion tick once its target can't carry",
    "[command_ids]") {
    for (const bool carries : {false, true}) {
        World w;
        Unit& f = w.factory(0.0f);
        Unit& t = w.walker(10.0f);
        if (carries) {
            t.add_category("TRANSPORTATION");
        }
        f.clear_rally_orders();
        UnitCommand board;
        board.type = CommandType::TransportLoad;
        board.target_id = t.entity_id();
        f.add_rally_order(board);
        w.ticks(1);
        REQUIRE(f.rally_orders().size() == 1);
        CHECK((f.rally_orders().front().type == CommandType::TransportLoad) == carries);
    }
}

TEST_CASE("No Rush keeps a factory from an initial rally out of its zone", "[command_ids]") {
    for (const bool no_rush : {false, true}) {
        World w;
        if (no_rush) {
            w.sim.set_no_rush(100.0f, 50.0f);
        }
        Unit& f = w.factory(100.0f);
        CHECK(f.rally_orders().empty() == no_rush);
    }
}

TEST_CASE("An edit of the sim's order finds it by its id in every unit that holds it",
          "[command_ids]") {
    World w;
    Unit& f = w.factory(0.0f);
    Unit& u = w.walker(10.0f);
    REQUIRE(f.rally_orders().size() == 1);
    const UnitCommand rally = f.rally_orders().front();
    u.push_command(rally, false);

    osc::sim::SimCallbackEntry cb;
    cb.func_name = osc::sim::kSetCommandTargetCallback;
    cb.args["Command"] = static_cast<double>(rally.command_id);
    cb.args["X"] = 30.0;
    cb.args["Y"] = 0.0;
    cb.args["Z"] = 40.0;
    w.sim.submit_callback(cb);
    w.ticks(1);
    CHECK(f.rally_orders().front().target_pos.x == 30.0f);
    CHECK(f.rally_orders().front().target_pos.z == 40.0f);
    REQUIRE_FALSE(u.command_queue().empty());
    CHECK(u.command_queue().front().target_pos.x == 30.0f);
}
