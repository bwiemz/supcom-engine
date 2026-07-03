// Replay tests: record the command stream, serialize it, and re-feed it into a
// fresh sim to reproduce the match bit-for-bit (via the sync checksum).

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/replay.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::CommandType;
using osc::sim::Replay;
using osc::sim::ScheduledCommand;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

// Identical initial world in both the recorded and replayed sims (a real replay
// gets this from the deterministic map/scenario setup).
std::vector<osc::u32> setup(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    std::vector<osc::u32> ids;
    for (int i = 0; i < 3; ++i) {
        auto u = std::make_unique<Unit>();
        u->set_army(i % 2);
        u->set_max_speed(6.0f);
        u->set_position({static_cast<osc::f32>(i * 20), 0.0f, 0.0f});
        ids.push_back(sim.entity_registry().register_entity(std::move(u)));
    }
    return ids;
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("Replay serialize/deserialize round-trips", "[replay]") {
    Replay r;
    r.final_tick = 42;
    r.command_delay = 2;
    r.victory_condition = "domination";

    ScheduledCommand a;
    a.exec_tick = 5;
    a.source = 1;
    a.clear_existing = true;
    a.command.type = CommandType::Move;
    a.command.target_pos = {12.5f, 0.0f, -7.25f};
    a.command.target_id = 99;
    a.command.blueprint_id = "uel0001";
    a.unit_ids = {3, 4, 5};
    r.commands.push_back(a);

    ScheduledCommand b;
    b.exec_tick = 9;
    b.source = 0;
    b.clear_existing = false;
    b.command.type = CommandType::Attack;
    b.command.target_id = 7;
    b.unit_ids = {8};
    r.commands.push_back(b);

    std::vector<osc::u8> bytes = r.serialize();
    Replay out;
    REQUIRE(Replay::deserialize(bytes, out));

    CHECK(out.version == Replay::kVersion);
    CHECK(out.final_tick == 42);
    CHECK(out.command_delay == 2);
    CHECK(out.victory_condition == "domination");
    REQUIRE(out.commands.size() == 2);

    CHECK(out.commands[0].exec_tick == 5);
    CHECK(out.commands[0].source == 1);
    CHECK(out.commands[0].clear_existing);
    CHECK(out.commands[0].command.type == CommandType::Move);
    CHECK(out.commands[0].command.target_pos.x == 12.5f);
    CHECK(out.commands[0].command.target_pos.z == -7.25f);
    CHECK(out.commands[0].command.target_id == 99);
    CHECK(out.commands[0].command.blueprint_id == "uel0001");
    CHECK(out.commands[0].unit_ids == std::vector<osc::u32>{3, 4, 5});

    CHECK(out.commands[1].command.type == CommandType::Attack);
    CHECK_FALSE(out.commands[1].clear_existing);
    CHECK(out.commands[1].unit_ids == std::vector<osc::u32>{8});
}

TEST_CASE("Replay deserialize rejects bad data", "[replay]") {
    Replay out;
    CHECK_FALSE(Replay::deserialize({}, out));                 // empty
    CHECK_FALSE(Replay::deserialize({'X', 'X', 'X', 'X'}, out)); // bad magic
    std::vector<osc::u8> truncated = {'O', 'S', 'C', 'R', 1};    // header cut off
    CHECK_FALSE(Replay::deserialize(truncated, out));
    CHECK(out.commands.empty());
}

TEST_CASE("Recording captures the scheduled command stream", "[replay]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto ids = setup(sim);
    sim.set_recording(true);

    sim.schedule_command(0, {ids[0]}, move_to(50.0f, 0.0f), true); // exec 1
    sim.tick();
    sim.tick();
    sim.schedule_command(1, {ids[1]}, move_to(0.0f, 50.0f), true); // exec 3

    const Replay& r = sim.recorded_replay();
    REQUIRE(r.commands.size() == 2);
    CHECK(r.commands[0].exec_tick == 1);
    CHECK(r.commands[1].exec_tick == 3);
    CHECK(r.final_tick == 3);
}

TEST_CASE("A recorded replay reproduces the match", "[replay][sync]") {
    // --- Record ---
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    auto ids = setup(a);
    a.set_recording(true);
    a.schedule_command(0, {ids[0]}, move_to(300.0f, 0.0f), true);
    for (int i = 0; i < 5; ++i) a.tick();
    a.schedule_command(1, {ids[1]}, move_to(0.0f, 300.0f), true);
    a.schedule_command(0, {ids[2]}, move_to(-200.0f, 50.0f), false);
    for (int i = 0; i < 25; ++i) a.tick();
    osc::u32 checksum_a = a.compute_sync_checksum();

    // Persist and reload the replay (full pipeline).
    std::vector<osc::u8> bytes = a.recorded_replay().serialize();
    Replay loaded;
    REQUIRE(Replay::deserialize(bytes, loaded));

    // --- Replay into a fresh sim with the same deterministic setup ---
    LuaGuard gb;
    SimState b(gb.L, nullptr);
    auto ids_b = setup(b);
    REQUIRE(ids_b == ids); // same id assignment
    b.queue_replay(loaded);
    for (int i = 0; i < 30; ++i) b.tick();

    CHECK(b.compute_sync_checksum() == checksum_a);
    // And the units actually moved (the replay did real work).
    auto* u = static_cast<Unit*>(b.entity_registry().find(ids[0]));
    CHECK(u->position().x > 0.0f);
}
