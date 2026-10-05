#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/navigator.hpp"
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
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

void flat(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
}

Unit* walker(SimState& sim, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_position({x, 0.0f, z});
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    u->add_command_cap("RULEUCC_Patrol");
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

osc::sim::UnitCommand patrol(osc::f32 x, osc::f32 z, osc::u32 id) {
    osc::sim::UnitCommand c;
    c.type = CommandType::Patrol;
    c.target_pos = {x, 0.0f, z};
    c.command_id = id;
    return c;
}

} // namespace

TEST_CASE("A patrol clicked once runs between the click and where it was given", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = walker(sim, 10.0f, 10.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({u->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Patrol";
    REQUIRE(input.click_in_command_mode(sim, mode, 60.0f, 10.0f, false));
    bool there = false;
    bool back = false;
    for (int t = 0; t < 600; ++t) {
        sim.tick();
        there |= u->position().x > 58.0f;
        back |= there && u->position().x < 12.0f;
    }
    CHECK(there);
    CHECK(back);
}

TEST_CASE("A patrol point added to a running patrol joins the loop after the last", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = walker(sim, 10.0f, 10.0f);
    u->push_command(patrol(60.0f, 10.0f, 1), true);
    u->push_command(patrol(60.0f, 60.0f, 2), false);
    for (int t = 0; t < 400 && u->command_queue().front().command_id != 2; ++t) {
        sim.tick();
    }
    REQUIRE(u->command_queue().front().command_id == 2);
    u->push_command(patrol(10.0f, 60.0f, 3), false);
    REQUIRE(u->command_queue().size() == 3);
    CHECK(u->command_queue()[1].command_id == 3);
    CHECK(u->command_queue()[2].command_id == 1);
}

TEST_CASE("A queued patrol starts where the orders before it end even before they run",
          "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* u = walker(sim, 10.0f, 10.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({u->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Patrol";
    REQUIRE_FALSE(input.right_click_at(sim, 60.0f, 60.0f, false).empty());
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 60.0f, true));
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 90.0f, true));
    sim.tick();
    const auto& q = u->command_queue();
    REQUIRE(q.size() == 4);
    CHECK(q[0].type == CommandType::Move);
    CHECK(q[1].type == CommandType::Patrol);
    CHECK(q[1].target_pos.x == 60.0f);
    CHECK(q[1].target_pos.z == 60.0f);
    CHECK(q[3].target_pos.x == 90.0f);
    CHECK(q[3].target_pos.z == 90.0f);

    osc::sim::UnitCommand stop;
    stop.type = CommandType::Stop;
    sim.set_human_input_active(true);
    sim.route_player_command({u->entity_id()}, stop, true);
    sim.set_human_input_active(false);
    const osc::sim::Vector3 at = u->position();
    REQUIRE(input.click_in_command_mode(sim, mode, 30.0f, 10.0f, true));
    sim.tick();
    REQUIRE(q.size() == 2);
    const auto anchored = [&](const osc::sim::UnitCommand& c) {
        return c.target_pos.x == at.x && c.target_pos.z == at.z;
    };
    CHECK((anchored(q[0]) || anchored(q[1])));
}

TEST_CASE("A queued patrol starts where the last unit with orders ends them", "[patrol]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    flat(sim);
    Unit* busy = walker(sim, 10.0f, 10.0f);
    Unit* idle = walker(sim, 10.0f, 30.0f);
    osc::sim::UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {60.0f, 0.0f, 60.0f};
    busy->push_command(move, true);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({busy->entity_id(), idle->entity_id()});
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Patrol";
    REQUIRE(input.click_in_command_mode(sim, mode, 90.0f, 10.0f, true));
    sim.tick();
    REQUIRE(idle->command_queue().size() == 2);
    CHECK(idle->command_queue()[0].target_pos.x == 60.0f);
    CHECK(idle->command_queue()[0].target_pos.z == 60.0f);
}
