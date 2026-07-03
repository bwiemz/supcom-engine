// Command scheduler tests — the deterministic / lockstep-ready command
// pipeline that underpins replay and multiplayer.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::CommandScheduler;
using osc::sim::CommandType;
using osc::sim::ScheduledCommand;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

osc::u32 spawn_mover(SimState& sim, int army, osc::f32 speed) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_max_speed(speed);
    u->set_position({0.0f, 0.0f, 0.0f});
    return sim.entity_registry().register_entity(std::move(u));
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

// ----- CommandScheduler in isolation -------------------------------------

TEST_CASE("Scheduler dispatches commands only on their exec tick", "[cmd][sched]") {
    CommandScheduler s;
    ScheduledCommand c;
    c.exec_tick = 5;
    c.source = 0;
    s.submit(c);

    std::vector<osc::u32> ran;
    for (osc::u32 t = 1; t <= 7; ++t)
        s.dispatch_due(t, [&](const ScheduledCommand&) { ran.push_back(t); });

    REQUIRE(ran.size() == 1);
    CHECK(ran[0] == 5);
    CHECK(s.pending_count() == 0); // consumed
}

TEST_CASE("Scheduler dispatch order is canonical (source, then sequence)",
          "[cmd][sched]") {
    CommandScheduler s;
    // Submit out of order and interleaved across sources, all for tick 3.
    auto mk = [](osc::u32 src, osc::u32 tag) {
        ScheduledCommand c;
        c.exec_tick = 3;
        c.source = src;
        c.command.command_id = tag; // reuse as a visible marker
        return c;
    };
    s.submit(mk(2, 100));
    s.submit(mk(0, 200));
    s.submit(mk(2, 101));
    s.submit(mk(0, 201));

    std::vector<osc::u32> order;
    s.dispatch_due(3, [&](const ScheduledCommand& c) {
        order.push_back(c.command.command_id);
    });

    // Source 0's two commands (in submission order), then source 2's.
    REQUIRE(order.size() == 4);
    CHECK(order[0] == 200);
    CHECK(order[1] == 201);
    CHECK(order[2] == 100);
    CHECK(order[3] == 101);
}

TEST_CASE("Lockstep gate waits for every source to confirm", "[cmd][sched]") {
    CommandScheduler s;
    s.set_lockstep(true);
    s.add_source(0);
    s.add_source(1);

    CHECK_FALSE(s.ready_to_run(4)); // nobody confirmed yet
    s.confirm_frame(0, 4);
    CHECK_FALSE(s.ready_to_run(4)); // source 1 still pending
    s.confirm_frame(1, 4);
    CHECK(s.ready_to_run(4));
    CHECK_FALSE(s.ready_to_run(5)); // next tick not yet confirmed
}

TEST_CASE("Non-lockstep scheduler never stalls", "[cmd][sched]") {
    CommandScheduler s;
    s.add_source(0);
    s.add_source(1);
    CHECK(s.ready_to_run(1));
    CHECK(s.ready_to_run(9999));
}

// ----- SimState integration ----------------------------------------------

TEST_CASE("Scheduled order lands in the unit queue on its exec tick",
          "[cmd][sim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    // Positive speed + far goal so the unit stays en route (order persists).
    osc::u32 id = spawn_mover(sim, 0, 5.0f);

    // Submitted before tick 1 → executes at tick_count+1 = 1.
    osc::u32 exec = sim.schedule_command(0, {id}, move_to(500.0f, 0.0f), true);
    CHECK(exec == 1);

    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    REQUIRE(u->command_queue().empty()); // not applied before its tick

    sim.tick(); // tick 1 — dispatch happens at the start
    REQUIRE(u->command_queue().size() == 1);
    CHECK(u->command_queue().front().type == CommandType::Move);
    CHECK(u->command_queue().front().target_pos.x == 500.0f);
}

TEST_CASE("command_delay defers execution", "[cmd][sim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_command_delay(3);
    osc::u32 id = spawn_mover(sim, 0, 5.0f);

    osc::u32 exec = sim.schedule_command(0, {id}, move_to(500.0f, 0.0f), true);
    CHECK(exec == 4); // tick_count(0) + 1 + delay(3)

    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));
    for (int i = 0; i < 3; ++i) {
        sim.tick();
        CHECK(u->command_queue().empty()); // still waiting
    }
    sim.tick(); // tick 4
    CHECK(u->command_queue().size() == 1);
}

TEST_CASE("clear_existing replaces, queued appends", "[cmd][sim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    // Far goals so nothing arrives / pops during the test.
    osc::u32 id = spawn_mover(sim, 0, 5.0f);
    auto* u = static_cast<Unit*>(sim.entity_registry().find(id));

    sim.schedule_command(0, {id}, move_to(100.0f, 0.0f), true);  // fresh
    sim.tick();
    REQUIRE(u->command_queue().size() == 1);

    sim.schedule_command(0, {id}, move_to(200.0f, 0.0f), false); // queued (append)
    sim.tick();
    REQUIRE(u->command_queue().size() == 2);

    sim.schedule_command(0, {id}, move_to(300.0f, 0.0f), true);  // replace
    sim.tick();
    REQUIRE(u->command_queue().size() == 1);
    CHECK(u->command_queue().front().target_pos.x == 300.0f);
}

TEST_CASE("Identical command streams keep two sims in sync (replay/determinism)",
          "[cmd][sim][sync]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);

    osc::u32 ida = spawn_mover(a, 0, 5.0f);
    osc::u32 idb = spawn_mover(b, 0, 5.0f);
    REQUIRE(ida == idb); // same id assignment → same stream applies to same unit

    // Feed both the identical command stream.
    a.schedule_command(0, {ida}, move_to(100.0f, 0.0f), true);
    b.schedule_command(0, {idb}, move_to(100.0f, 0.0f), true);

    for (int i = 0; i < 30; ++i) {
        a.tick();
        b.tick();
        CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());
    }
    // The order actually moved the unit (proving the pipeline is live, not a
    // no-op), so the checksum is a meaningful determinism witness.
    auto* ua = static_cast<Unit*>(a.entity_registry().find(ida));
    CHECK(ua->position().x > 0.0f);
}

TEST_CASE("A divergent command stream desyncs the checksum", "[cmd][sim][sync]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    osc::u32 ida = spawn_mover(a, 0, 5.0f);
    osc::u32 idb = spawn_mover(b, 0, 5.0f);

    a.schedule_command(0, {ida}, move_to(100.0f, 0.0f), true);
    b.schedule_command(0, {idb}, move_to(-100.0f, 0.0f), true); // different target

    for (int i = 0; i < 10; ++i) {
        a.tick();
        b.tick();
    }
    CHECK(a.compute_sync_checksum() != b.compute_sync_checksum());
}

TEST_CASE("ready_to_run_next_tick reflects the lockstep gate", "[cmd][sim]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    CHECK(sim.ready_to_run_next_tick()); // single-player: never stalls

    sim.command_scheduler().set_lockstep(true);
    sim.command_scheduler().add_source(0);
    sim.command_scheduler().add_source(1);
    CHECK_FALSE(sim.ready_to_run_next_tick()); // waiting on peers

    sim.command_scheduler().confirm_frame(0, sim.tick_count() + 1);
    sim.command_scheduler().confirm_frame(1, sim.tick_count() + 1);
    CHECK(sim.ready_to_run_next_tick());
}
