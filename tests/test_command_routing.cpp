// Tests for SimState::route_command — the seam that keeps single-player orders
// direct while sending a networked client's local human orders to the lockstep
// sink. This is the decision the Issue* bindings and player-input path rely on.

#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
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

TEST_CASE("route_command applies directly in single-player", "[routing]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto id = spawn_unit(sim);

    REQUIRE_FALSE(sim.multiplayer());
    sim.route_command({id}, move_to(100.0f, 0.0f), true);

    // No sink installed: the order lands on the unit immediately.
    CHECK(unit_of(sim, id)->command_queue().size() == 1);
    CHECK(unit_of(sim, id)->command_queue().front().type == CommandType::Move);
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
    CHECK(unit_of(sim, id)->command_queue().size() == 1); // direct again
}
