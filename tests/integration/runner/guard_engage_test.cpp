// --guard-engage-test: a guard takes on an enemy near it and goes home
// after, as Moho's CUnitGuardTask does, and a guard of a point (retail's AI
// guards bases and markers so) goes there.
//
// A UEF T1 tank guards a T2 tank. An enemy engineer stands 23 from the
// guard: within its GuardScanRadius (25), beyond its weapon's reach (20.7)
// and its sight (20); an engineer of its army, 12 off, sees it (a guard, as
// Moho's, takes on only what its army has a blip of).
// 1. The guard breaks off to attack it, still Guarding (and Attacking) and
//    still guarding the T2 tank, as scripts see it.
// 2. It kills it, and its Guard is its order again.
// 3. It goes home to the T2 tank.
// 4. An idle T1 tank 23 from an enemy engineer of its own, seen the same
//    way, never moves: only the guard looks so far.
// 5. IssueGuard at a position: the tank goes there and guards it, Guarding
//    with no guarded unit.
//
// Before, a guard only followed, and a Guard of a position was dropped.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

void test_guard_engage(TestContext& ctx) {
    spdlog::info("=== GUARD ENGAGE TEST: a guard takes on an enemy near it ===");
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
    // `code` (Lua) passes when it runs without an error.
    const auto lua_ok = [&](const std::string& code) {
        return static_cast<bool>(ctx.lua_state.do_string(code));
    };

    const u32 ward_id = spawn_unit(ctx, "__osc_ge_ward", "uel0202", "ARMY_1", {sx, sz});
    const u32 guard_id = spawn_unit(ctx, "__osc_ge_guard", "uel0201", "ARMY_1", {sx + 5, sz});
    const u32 enemy_id = spawn_unit(ctx, "__osc_ge_enemy", "uel0105", "ARMY_2", {sx + 28, sz});
    const u32 idle_id = spawn_unit(ctx, "__osc_ge_idle", "uel0201", "ARMY_1", {sx, sz + 50});
    const u32 bait_id = spawn_unit(ctx, "__osc_ge_bait", "uel0105", "ARMY_2", {sx + 23, sz + 50});
    spawn_unit(ctx, "__osc_ge_spot1", "uel0105", "ARMY_1", {sx + 28, sz + 12});
    spawn_unit(ctx, "__osc_ge_spot2", "uel0105", "ARMY_1", {sx + 23, sz + 62});
    sim::Unit* guard = unit(guard_id);
    if (!guard || !unit(ward_id) || !unit(enemy_id) || !unit(idle_id) || !unit(bait_id)) {
        t.check(false, "the units spawn");
        return;
    }
    const sim::Vector3 idle_at = unit(idle_id)->position();
    run_lua(ctx, "IssueGuard({__osc_ge_guard}, __osc_ge_ward)");

    bool engaged = false;
    for (int i = 0; i < 40 && !engaged && unit(guard_id); ++i) {
        ctx.sim.tick();
        const auto& q = unit(guard_id)->command_queue();
        engaged = !q.empty() && q.front().from_guard &&
                  q.front().type == sim::CommandType::Attack && q.front().target_id == enemy_id;
    }
    t.check(engaged, "Test 1: the guard breaks off to attack the engineer 23 off");
    t.check(engaged && lua_ok("if not __osc_ge_guard:IsUnitState('Guarding') or "
                              "not __osc_ge_guard:IsUnitState('Attacking') or "
                              "__osc_ge_guard:GetGuardedUnit() ~= __osc_ge_ward then "
                              "error('states') end"),
            "Test 1: fighting, it is Guarding and Attacking, and guards the T2 tank");

    bool killed = false;
    for (int i = 0; i < 600 && !killed; ++i) {
        ctx.sim.tick();
        killed = !unit(enemy_id);
    }
    guard = unit(guard_id);
    t.check(killed && guard && !guard->command_queue().empty() &&
                guard->command_queue().front().type == sim::CommandType::Guard &&
                guard->command_queue().front().target_id == ward_id,
            "Test 2: it kills the engineer, and its Guard is its order again");

    bool home = false;
    for (int i = 0; i < 300 && !home && guard; ++i) {
        ctx.sim.tick();
        guard = unit(guard_id);
        const sim::Unit* ward = unit(ward_id);
        home = guard && ward &&
               std::hypot(guard->position().x - ward->position().x,
                          guard->position().z - ward->position().z) <= 10.5f &&
               !guard->command_queue().front().guard_returning;
    }
    t.check(home, "Test 3: it goes home to the T2 tank");

    const sim::Unit* idle = unit(idle_id);
    const f32 idle_moved =
        idle ? std::hypot(idle->position().x - idle_at.x, idle->position().z - idle_at.z) : 1e9f;
    t.check(idle && idle_moved < 0.5f && unit(bait_id),
            fmt::format("Test 4: an idle tank 23 from an engineer stays put ({:.2f})", idle_moved));

    run_lua(ctx,
            fmt::format("__osc_ge_bait:Destroy()\n"
                        "IssueGuard({{__osc_ge_idle}}, {{{0}, GetTerrainHeight({0}, {1}), {1}}})",
                        sx + 20, sz - 30));
    bool there = false;
    for (int i = 0; i < 600 && !there && unit(idle_id); ++i) {
        ctx.sim.tick();
        const sim::Unit* u = unit(idle_id);
        there = !u->command_queue().empty() &&
                u->command_queue().front().type == sim::CommandType::Guard &&
                u->command_queue().front().target_id == 0 &&
                std::hypot(u->position().x - (sx + 20), u->position().z - (sz - 30)) <= 2.5f;
    }
    t.check(there && lua_ok("if not __osc_ge_idle:IsUnitState('Guarding') or "
                            "__osc_ge_idle:GetGuardedUnit() ~= nil then error('states') end"),
            "Test 5: IssueGuard at a position: the tank goes there and guards it");

    spdlog::info("=== GUARD ENGAGE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
