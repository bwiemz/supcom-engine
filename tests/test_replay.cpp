// Replay tests: record the command stream, serialize it, and re-feed it into a
// fresh sim to reproduce the match bit-for-bit (via the sync checksum).

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/command_codec.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/net_transport.hpp"
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

TEST_CASE("Recording keeps what the sim applies, and a checksum each tick", "[replay]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto ids = setup(sim);
    sim.set_recording(true);

    sim.schedule_command(0, {ids[0]}, move_to(50.0f, 0.0f), true); // exec 1
    sim.tick();
    sim.tick();
    sim.schedule_command(1, {ids[1]}, move_to(0.0f, 50.0f), true); // exec 3, not run

    const Replay& r = sim.recorded_replay();
    REQUIRE(r.commands.size() == 1); // only what ran
    CHECK(r.commands[0].exec_tick == 1);
    CHECK(r.final_tick == 2);
    CHECK(r.checksum_from == 1);
    REQUIRE(r.checksums.size() == 2);
    CHECK(r.checksums[1] == sim.compute_sync_checksum());
    CHECK_FALSE(r.build.empty());

    sim.tick();
    CHECK(sim.recorded_replay().commands.size() == 2);
    CHECK(sim.recorded_replay().commands[1].exec_tick == 3);
}

TEST_CASE("A lockstep peer's recording holds the other peers' commands", "[replay][lockstep]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    auto ids = setup(a);
    setup(b);
    b.set_recording(true);
    osc::sim::LoopbackHub hub;
    osc::sim::LoopbackTransport ta(hub, hub.add_endpoint());
    osc::sim::LoopbackTransport tb(hub, hub.add_endpoint());
    osc::sim::LockstepSession sa(a, ta, 0, {0, 1});
    osc::sim::LockstepSession sb(b, tb, 1, {0, 1});
    a.set_local_command_sink([&](const std::vector<osc::u32>& u, const UnitCommand& c, bool clear) {
        sa.submit_local(u, c, clear);
    });
    for (int round = 0; round < 6; ++round) {
        if (round == 1) {
            a.set_human_input_active(true); // A's player orders a move
            a.route_command({ids[0]}, move_to(80.0f, 0.0f), true);
            a.set_human_input_active(false);
        }
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
    REQUIRE(b.recorded_replay().commands.size() == 1);
    CHECK(b.recorded_replay().commands[0].source == 0);
}

TEST_CASE("Playback checks every tick against the recording", "[replay][sync]") {
    Replay recorded;
    std::vector<osc::u32> ids;
    {
        LuaGuard g;
        SimState a(g.L, nullptr);
        a.set_seed(99);
        ids = setup(a);
        a.set_recording(true);
        a.schedule_command(0, {ids[0]}, move_to(300.0f, 0.0f), true);
        for (int i = 0; i < 10; ++i) a.tick();
        a.set_human_input_active(true);
        a.route_command({ids[1]}, move_to(0.0f, 300.0f), true);
        a.set_human_input_active(false);
        for (int i = 0; i < 20; ++i) a.tick();
        REQUIRE(Replay::deserialize(a.recorded_replay().serialize(), recorded));
    }
    auto play = [&](const Replay& replay) {
        LuaGuard g;
        SimState b(g.L, nullptr);
        b.set_seed(replay.seed);
        setup(b);
        osc::sim::ReplayPlayback playback(replay);
        playback.start(b);
        // A player's order during playback is not part of the game.
        b.set_human_input_active(true);
        b.route_command({ids[2]}, move_to(-300.0f, 0.0f), true);
        b.set_human_input_active(false);
        while (!playback.finished(b)) {
            b.tick();
            if (!playback.check(b)) break;
        }
        return playback.diverged_at();
    };
    CHECK(play(recorded) == 0);

    // A replay whose order differs diverges when the order runs.
    Replay tampered = recorded;
    REQUIRE(tampered.commands.size() == 2);
    tampered.commands[1].command.target_pos.x = 50.0f;
    CHECK(play(tampered) == tampered.commands[1].exec_tick);
}

TEST_CASE("A replay carries the game's setup", "[replay]") {
    Replay r;
    r.has_setup = true;
    r.setup.scenario = "/maps/SCMP_009/SCMP_009_scenario.lua";
    r.setup.seed = 4242;
    r.setup.army_count = 2;
    osc::sim::ArmySlotConfig human;
    human.configured = true;
    human.faction = 3;
    human.team = 2;
    human.start_spot = 2;
    human.player_color = -1;
    osc::sim::ArmySlotConfig ai = human;
    ai.human = false;
    ai.ai_personality = "rush";
    r.setup.slots = {human, ai};
    r.setup.options.configured = true;
    r.setup.options.set_string("Victory", "demoralization");
    r.setup.options.set_number("UnitCap", 500);
    r.setup.options.set_bool("CheatsEnabled", false);
    r.setup.options.restricted_categories = {"NUKE"};
    r.setup.ai_armies = {1};
    r.setup.cheat_mult = 2.0;
    r.checksum_from = 1;
    r.checksums = {0xdeadbeef, 0x12345678};

    Replay out;
    REQUIRE(Replay::deserialize(r.serialize(), out));
    REQUIRE(out.has_setup);
    CHECK(out.setup.scenario == r.setup.scenario);
    CHECK(out.setup.seed == 4242);
    CHECK(out.setup.army_count == 2);
    REQUIRE(out.setup.slots.size() == 2);
    CHECK(out.setup.slots[0].faction == 3);
    CHECK(out.setup.slots[0].player_color == -1);
    CHECK_FALSE(out.setup.slots[1].human);
    CHECK(out.setup.slots[1].ai_personality == "rush");
    REQUIRE(out.setup.options.values.size() == 3);
    CHECK(out.setup.options.values[1].first == "UnitCap");
    CHECK(out.setup.options.values[1].second.number_value == 500);
    CHECK(out.setup.options.values[2].second.type == osc::sim::GameOptionValue::Type::Boolean);
    CHECK(out.setup.options.restricted_categories == std::vector<std::string>{"NUKE"});
    CHECK(out.setup.ai_armies == std::vector<int>{1});
    CHECK(out.setup.cheat_mult == 2.0);
    CHECK(out.checksums == r.checksums);

    // Cut short anywhere, it is refused whole.
    auto bytes = r.serialize();
    for (size_t cut : {bytes.size() - 1, bytes.size() / 2, size_t{20}}) {
        std::vector<osc::u8> part(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(Replay::deserialize(part, out));
    }
}

TEST_CASE("A version 3 replay still loads, without a setup", "[replay]") {
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    for (char c : {'O', 'S', 'C', 'R'}) w.u8v(static_cast<osc::u8>(c));
    w.u32v(3);  // version
    w.u32v(12); // final tick
    w.u32v(0);  // command delay
    w.u64v(77); // seed
    w.str("domination");
    w.u32v(0); // commands
    Replay out;
    REQUIRE(Replay::deserialize(bytes, out));
    CHECK(out.version == 3);
    CHECK(out.seed == 77);
    CHECK_FALSE(out.has_setup);
    CHECK(out.checksums.empty());
}

TEST_CASE("A formation order round-trips; a version 4 command loads without one", "[replay]") {
    Replay r;
    ScheduledCommand c;
    c.exec_tick = 5;
    c.command.type = CommandType::Move;
    c.command.target_pos = {10.0f, 0.0f, 20.0f};
    c.command.formation = "AttackFormation";
    c.command.has_facing = true;
    c.command.facing = 1.5f;
    c.unit_ids = {3, 4};
    r.commands.push_back(c);
    Replay out;
    REQUIRE(Replay::deserialize(r.serialize(), out));
    REQUIRE(out.commands.size() == 1);
    CHECK(out.commands[0].command.formation == "AttackFormation");
    CHECK(out.commands[0].command.has_facing);
    CHECK(out.commands[0].command.facing == 1.5f);
    CHECK(out.commands[0].unit_ids == std::vector<osc::u32>{3, 4});

    // Version 4 wrote no formation fields.
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    for (char ch : {'O', 'S', 'C', 'R'}) w.u8v(static_cast<osc::u8>(ch));
    w.u32v(4);  // version
    w.u32v(12); // final tick
    w.u32v(0);  // command delay
    w.u64v(77); // seed
    w.str("domination");
    w.str("build");
    w.u8v(0);  // no setup
    w.u32v(0); // checksums from
    w.u32v(0); // no checksums
    w.u32v(1); // one command
    w.u32v(5); // exec tick
    w.u32v(0); // source
    w.u8v(1);  // clear
    w.u8v(static_cast<osc::u8>(CommandType::Move));
    w.f32v(10.0f);
    w.f32v(0.0f);
    w.f32v(20.0f);
    w.u32v(0); // target id
    w.u32v(9); // command id
    w.str(""); // blueprint
    w.u32v(1); // one unit
    w.u32v(3);
    w.u8v(0); // no callback
    REQUIRE(Replay::deserialize(bytes, out));
    REQUIRE(out.commands.size() == 1);
    CHECK(out.commands[0].command.formation.empty());
    CHECK(out.commands[0].unit_ids == std::vector<osc::u32>{3});
}

TEST_CASE("A recorded replay reproduces the match", "[replay][sync]") {
    // --- Record ---
    LuaGuard ga;
    SimState a(ga.L, nullptr);
    a.set_seed(777); // a game's own seed, not the default
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
    CHECK(loaded.seed == 777);

    // --- Replay into a fresh sim seeded as the recording was, before any
    // of it runs (boot scripts roll numbers too) ---
    LuaGuard gb;
    SimState b(gb.L, nullptr);
    b.set_seed(loaded.seed);
    auto ids_b = setup(b);
    REQUIRE(ids_b == ids); // same id assignment
    b.queue_replay(loaded);
    for (int i = 0; i < 30; ++i) b.tick();

    CHECK(b.compute_sync_checksum() == checksum_a);
    // And the units actually moved (the replay did real work).
    auto* u = static_cast<Unit*>(b.entity_registry().find(ids[0]));
    CHECK(u->position().x > 0.0f);
}

TEST_CASE("A version 1 replay (no seed) still loads, with the default seed", "[replay]") {
    Replay r;
    r.final_tick = 9;
    r.command_delay = 2;
    r.victory_condition = "demoralization";
    std::vector<osc::u8> bytes = r.serialize();
    // Version 1 had no seed: drop the 8 bytes after command_delay and
    // rewrite the version.
    constexpr size_t kSeedAt = 4 + 4 + 4 + 4; // magic, version, final_tick, command_delay
    bytes.erase(bytes.begin() + kSeedAt, bytes.begin() + kSeedAt + 8);
    bytes[4] = 1;

    Replay loaded;
    REQUIRE(Replay::deserialize(bytes, loaded));
    CHECK(loaded.final_tick == 9);
    CHECK(loaded.command_delay == 2);
    CHECK(loaded.victory_condition == "demoralization");
    CHECK(loaded.seed == osc::sim::SimRandom::kDefaultSeed);
}
