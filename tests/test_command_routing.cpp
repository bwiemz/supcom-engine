// Tests for SimState::route_command — the seam that schedules a player's
// orders (single-player) or sends them to the lockstep sink (multiplayer),
// while AI and script orders, issued inside a tick, apply directly. This is
// the decision the Issue* bindings and player-input path rely on.

#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <vector>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

osc::u32 spawn_unit(SimState& sim) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(6.0f); // so a Move lasts past the tick it starts in
    return sim.entity_registry().register_entity(std::move(u));
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

Unit* unit_of(SimState& sim, osc::u32 id) {
    return static_cast<Unit*>(sim.entity_registry().find(id));
}

} // namespace

TEST_CASE("route_command applies an AI or script order directly in single-player", "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);

    REQUIRE_FALSE(sim.multiplayer());
    sim.route_command({id}, move_to(100.0f, 0.0f), true);

    // Issued inside a tick (not human input): it lands on the unit at once.
    CHECK(unit_of(sim, id)->command_queue().size() == 1);
    CHECK(unit_of(sim, id)->command_queue().front().type == CommandType::Move);
}

TEST_CASE("A single-player player's order applies in the next tick, and is recorded",
          "[routing][replay]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);
    sim.set_recording(true);

    sim.set_human_input_active(true);
    sim.route_command({id}, move_to(100.0f, 0.0f), true);
    sim.set_human_input_active(false);

    // Not between ticks: Moho applies orders in the sim, as commands.
    CHECK(unit_of(sim, id)->command_queue().empty());
    sim.tick();
    REQUIRE(unit_of(sim, id)->command_queue().size() == 1);
    CHECK(unit_of(sim, id)->command_queue().front().type == CommandType::Move);
    CHECK(unit_of(sim, id)->command_queue().front().command_id != 0); // numbered in-tick
    REQUIRE(sim.recorded_replay().commands.size() == 1);
    CHECK(sim.recorded_replay().commands[0].exec_tick == 1);
}

TEST_CASE("route_command sends local human orders to the sink in multiplayer",
          "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);

    std::vector<osc::u32> captured_ids;
    UnitCommand captured_cmd;
    bool captured_clear = false;
    int sink_calls = 0;
    sim.set_local_command_sink(
        [&](const std::vector<osc::u32>& ids, const UnitCommand& cmd, bool clear) {
            captured_ids = ids;
            captured_cmd = cmd;
            captured_clear = clear;
            ++sink_calls;
        });

    REQUIRE(sim.multiplayer());

    // With human input active, the order is handed to the network sink and NOT
    // applied to the local unit directly (the session schedules it instead).
    sim.set_human_input_active(true);
    sim.route_command({id}, move_to(250.0f, 0.0f), true);

    CHECK(sink_calls == 1);
    CHECK(captured_ids == std::vector<osc::u32>{id});
    CHECK(captured_cmd.type == CommandType::Move);
    CHECK(captured_clear == true);
    CHECK(unit_of(sim, id)->command_queue().empty()); // not applied locally
}

TEST_CASE("route_command keeps AI/sim orders direct even in multiplayer",
          "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);

    int sink_calls = 0;
    sim.set_local_command_sink(
        [&](const std::vector<osc::u32>&, const UnitCommand&, bool) {
            ++sink_calls;
        });
    REQUIRE(sim.multiplayer());

    // Human input NOT active → this is a deterministic AI/sim order. It must
    // apply directly on every client and never be broadcast.
    sim.set_human_input_active(false);
    sim.route_command({id}, move_to(300.0f, 0.0f), true);

    CHECK(sink_calls == 0);
    CHECK(unit_of(sim, id)->command_queue().size() == 1);
}

TEST_CASE("route_command Stop clears the queue directly in single-player",
          "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);

    // Give the unit an order, then Stop it.
    sim.route_command({id}, move_to(100.0f, 0.0f), true);
    REQUIRE(unit_of(sim, id)->command_queue().size() == 1);

    UnitCommand stop;
    stop.type = CommandType::Stop;
    sim.route_command({id}, stop, true);

    // Stop clears the queue outright (not a queued Stop order).
    CHECK(unit_of(sim, id)->command_queue().empty());
}

TEST_CASE("route_command Stop goes to the sink for a networked human", "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);
    sim.route_command({id}, move_to(100.0f, 0.0f), true);

    CommandType captured_type = CommandType::Move;
    int sink_calls = 0;
    sim.set_local_command_sink(
        [&](const std::vector<osc::u32>&, const UnitCommand& cmd, bool) {
            captured_type = cmd.type;
            ++sink_calls;
        });
    sim.set_human_input_active(true);

    UnitCommand stop;
    stop.type = CommandType::Stop;
    sim.route_command({id}, stop, true);

    // Broadcast to peers; not cleared locally (the session schedules it).
    CHECK(sink_calls == 1);
    CHECK(captured_type == CommandType::Stop);
    CHECK(unit_of(sim, id)->command_queue().size() == 1);
}

TEST_CASE("clear_local_command_sink returns to single-player behavior",
          "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);

    sim.set_local_command_sink(
        [&](const std::vector<osc::u32>&, const UnitCommand&, bool) {});
    sim.set_human_input_active(true);
    REQUIRE(sim.multiplayer());

    sim.clear_local_command_sink();
    CHECK_FALSE(sim.multiplayer());
    sim.route_command({id}, move_to(400.0f, 0.0f), true);
    CHECK(unit_of(sim, id)->command_queue().empty()); // scheduled, not sent
    sim.tick();
    CHECK(unit_of(sim, id)->command_queue().size() == 1);
}

TEST_CASE("A mapped source's orders move only its own army's units", "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const auto mine = spawn_unit(sim); // army 0
    const auto theirs = spawn_unit(sim);
    unit_of(sim, theirs)->set_army(1);
    sim.set_recording(true);

    // Source 1 plays army 1; its frame names a unit of army 0 as well.
    sim.set_source_army(1, 1);
    sim.schedule_command(1, {mine, theirs}, move_to(100.0f, 0.0f), true);
    sim.tick();
    CHECK(unit_of(sim, mine)->command_queue().empty());
    REQUIRE(unit_of(sim, theirs)->command_queue().size() == 1);
    // The recording keeps the order as applied, so a replay needs no mapping.
    REQUIRE(sim.recorded_replay().commands.size() == 1);
    CHECK(sim.recorded_replay().commands[0].unit_ids == std::vector<osc::u32>{theirs});

    // An unmapped source (the single-player player) is not limited.
    sim.schedule_command(0, {mine, theirs}, move_to(50.0f, 0.0f), true);
    sim.tick();
    CHECK(unit_of(sim, mine)->command_queue().size() == 1);
}

TEST_CASE("A mapped source's UI callbacks touch only its own army's units", "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const auto mine = spawn_unit(sim); // army 0
    const auto theirs = spawn_unit(sim);
    unit_of(sim, theirs)->set_army(1);

    // Source 1 (army 1) sets hold fire on its selection, which names a unit
    // of army 0 too.
    sim.set_source_army(1, 1);
    osc::sim::SimCallbackEntry hold;
    hold.func_name = osc::sim::kUnitSettingCallback;
    hold.args["Setting"] = std::string("FireState");
    hold.args["Value"] = 1.0;
    hold.unit_ids = {mine, theirs};
    sim.schedule_callback(1, hold);
    sim.tick();
    CHECK(unit_of(sim, mine)->fire_state() == 0);
    CHECK(unit_of(sim, theirs)->fire_state() == 1);
}
