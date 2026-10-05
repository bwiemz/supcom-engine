// --attack-move-test: an attack-move (IssueAggressiveMove) is Moho's
// UNITCOMMAND_AggressiveMove, one leg of a patrol task: the units fight
// what they meet near the way, then go on, and the order ends where they
// arrive. Retail's AI attack-moves everywhere (platoon:
// AggressiveMoveToLocation, IssueFormAggressiveMove), and these had been
// plain moves.
//
// Three UEF T1 tanks attack-move 100 north past two enemy engineers 15 off
// their way:
// 1. their order is an AggressiveMove, and scripts see them Patrolling;
// 2. they break off to attack, and both engineers die;
// 3. they get there, and their queues end empty: the order is not kept, as
//    a patrol's is.
// 4. IssueFormAggressiveMove and platoon:AggressiveMoveToLocation issue
//    AggressiveMove too, the first laid out in its formation's slots.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <string>

namespace osc::test {

void test_attack_move(TestContext& ctx) {
    spdlog::info("=== ATTACK MOVE TEST: attack-moves fight on the way, then end ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](u32 id) {
        sim::Entity* e = registry.find(id);
        return e && e->is_unit() && !e->destroyed() ? static_cast<sim::Unit*>(e) : nullptr;
    };
    const auto lua_ok = [&](const std::string& code) {
        return static_cast<bool>(ctx.lua_state.do_string(code));
    };

    const std::array<u32, 3> tanks = {
        spawn_unit(ctx, "__osc_am_t1", "uel0201", "ARMY_1", {sx - 25, sz - 35}),
        spawn_unit(ctx, "__osc_am_t2", "uel0201", "ARMY_1", {sx - 20, sz - 35}),
        spawn_unit(ctx, "__osc_am_t3", "uel0201", "ARMY_1", {sx - 15, sz - 35}),
    };
    const std::array<u32, 2> enemies = {
        spawn_unit(ctx, "__osc_am_e1", "uel0105", "ARMY_2", {sx - 5, sz + 10}),
        spawn_unit(ctx, "__osc_am_e2", "uel0105", "ARMY_2", {sx - 5, sz + 20}),
    };
    for (const u32 id : tanks)
        if (!unit(id)) {
            t.check(false, "the tanks spawn");
            return;
        }
    const f32 gx = sx - 20;
    const f32 gz = sz + 65;
    run_lua(ctx, fmt::format("IssueAggressiveMove({{__osc_am_t1, __osc_am_t2, __osc_am_t3}}, "
                             "{{{0}, GetTerrainHeight({0}, {1}), {1}}})",
                             gx, gz));
    ctx.sim.tick();
    const auto& first = unit(tanks[0])->command_queue();
    t.check(!first.empty() && first.front().type == sim::CommandType::AggressiveMove &&
                lua_ok("if not __osc_am_t1:IsUnitState('Patrolling') then error('state') end"),
            "Test 1: the order is an AggressiveMove, and scripts see the tanks Patrolling");

    bool broke_off = false;
    bool arrived = false;
    for (int i = 0; i < 1200 && !arrived; ++i) {
        ctx.sim.tick();
        arrived = true;
        for (const u32 id : tanks) {
            const sim::Unit* u = unit(id);
            if (!u) continue;
            const auto& q = u->command_queue();
            if (!q.empty() && q.front().from_patrol && q.front().type == sim::CommandType::Attack)
                broke_off = true;
            arrived = arrived && q.empty();
        }
    }
    t.check(broke_off && !unit(enemies[0]) && !unit(enemies[1]),
            fmt::format("Test 2: they break off to attack ({}), and both engineers die ({}, {})",
                        broke_off, !unit(enemies[0]), !unit(enemies[1])));
    f32 farthest = 0;
    for (const u32 id : tanks)
        if (const sim::Unit* u = unit(id))
            farthest = std::max(farthest, std::hypot(u->position().x - gx, u->position().z - gz));
    t.check(
        arrived && farthest < 8.0f,
        fmt::format("Test 3: they get there ({:.1f} off at most), their queues empty", farthest));

    run_lua(ctx, fmt::format("IssueClearCommands({{__osc_am_t1, __osc_am_t2, __osc_am_t3}})\n"
                             "IssueFormAggressiveMove({{__osc_am_t1, __osc_am_t2, __osc_am_t3}}, "
                             "{{{0}, GetTerrainHeight({0}, {1}), {1}}}, 'AttackFormation', 0)",
                             sx + 20, sz - 20));
    ctx.sim.tick();
    int formed = 0;
    std::array<f32, 3> xs{};
    for (size_t i = 0; i < tanks.size(); ++i) {
        const sim::Unit* u = unit(tanks[i]);
        if (!u || u->command_queue().empty()) continue;
        const sim::UnitCommand& c = u->command_queue().front();
        if (c.type == sim::CommandType::AggressiveMove && c.speed_cap > 0) ++formed;
        xs[i] = c.target_pos.x * 1000.0f + c.target_pos.z;
    }
    const bool slots = xs[0] != xs[1] && xs[1] != xs[2] && xs[0] != xs[2];
    run_lua(ctx, fmt::format("local brain = GetArmyBrain('ARMY_1')\n"
                             "local p = brain:MakePlatoon('attack-move test', '')\n"
                             "brain:AssignUnitsToPlatoon(p, {{__osc_am_t1}}, 'Attack', 'None')\n"
                             "IssueClearCommands({{__osc_am_t1}})\n"
                             "p:AggressiveMoveToLocation({{{0}, GetTerrainHeight({0}, {1}), {1}}})",
                             sx, sz));
    ctx.sim.tick();
    const sim::Unit* t1 = unit(tanks[0]);
    const bool platoon = t1 && !t1->command_queue().empty() &&
                         t1->command_queue().front().type == sim::CommandType::AggressiveMove;
    t.check(formed == 3 && slots && platoon,
            fmt::format("Test 4: IssueFormAggressiveMove lays out 3 AggressiveMoves in slots "
                        "({}, {}), platoon:AggressiveMoveToLocation issues one ({})",
                        formed, slots, platoon));

    spdlog::info("=== ATTACK MOVE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
