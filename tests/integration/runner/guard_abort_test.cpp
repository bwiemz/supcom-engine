// --guard-abort-test: Moho's CUnitGuardTask::ShouldAbortGuardForBuilderContext.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

void test_guard_abort(TestContext& ctx) {
    spdlog::info("=== GUARD ABORT TEST: an engineer's guard ends ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](sim::EntityId id) {
        sim::Entity* e = registry.find(id);
        return e && e->is_unit() && !e->destroyed() ? static_cast<sim::Unit*>(e) : nullptr;
    };
    const auto guarding = [&](sim::EntityId id) {
        const sim::Unit* u = unit(id);
        return u && !u->command_queue().empty() &&
               u->command_queue().front().type == sim::CommandType::Guard;
    };

    spawn_unit(ctx, "__osc_ga_fac1", "ueb0101", "ARMY_1", {sx, sz});
    const sim::EntityId eng1 = spawn_unit(ctx, "__osc_ga_eng1", "uel0105", "ARMY_1", {sx, sz + 9});
    spawn_unit(ctx, "__osc_ga_fac2", "ueb0101", "ARMY_1", {sx + 30, sz});
    const sim::EntityId eng2 =
        spawn_unit(ctx, "__osc_ga_eng2", "uel0105", "ARMY_1", {sx + 30, sz + 9});
    spawn_unit(ctx, "__osc_ga_b", "uel0105", "ARMY_1", {sx - 26, sz});
    const sim::EntityId eng3 =
        spawn_unit(ctx, "__osc_ga_eng3", "uel0105", "ARMY_1", {sx - 20, sz + 7});
    const sim::EntityId ward4 =
        spawn_unit(ctx, "__osc_ga_ward4", "uel0105", "ARMY_1", {sx + 30, sz + 40});
    const sim::EntityId eng4 =
        spawn_unit(ctx, "__osc_ga_eng4", "uel0105", "ARMY_1", {sx + 4, sz + 34});
    if (!unit(eng1) || !unit(eng2) || !unit(eng3) || !unit(ward4) || !unit(eng4)) {
        t.check(false, "the units spawn");
        return;
    }
    run_lua(ctx, fmt::format(R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 50000)
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('MASS', 50000)
        brain:GiveResource('ENERGY', 500000)
        IssueBuildMobile({{__osc_ga_b}}, {{{0}, GetSurfaceHeight({0}, {1}), {1}}}, 'ueb1101', {{}})
    )",
                             sx - 20, sz));
    if (!tick_until(ctx, "setup: an engineer begins a power generator", 300,
                    "__osc_ga_b:GetFocusUnit() and __osc_ga_b:GetFocusUnit():GetFractionComplete() "
                    "> 0.1")) {
        return;
    }
    run_lua(ctx, fmt::format(R"(
        __osc_ga_pgen = __osc_ga_b:GetFocusUnit()
        IssueClearCommands({{__osc_ga_b}})
        IssueMove({{__osc_ga_b}}, {{{0}, 0, {1}}})
        IssueGuard({{__osc_ga_eng3}}, __osc_ga_pgen)
        IssueBuildFactory({{__osc_ga_fac1}}, 'uel0201', 1)
        IssueBuildFactory({{__osc_ga_fac2}}, 'uel0201', 1)
        IssueGuard({{__osc_ga_eng1}}, __osc_ga_fac1)
        IssueBuildMobile({{__osc_ga_eng1}}, {{{2}, GetSurfaceHeight({2}, {3}), {3}}}, 'ueb1101', {{}})
        IssueGuard({{__osc_ga_eng2}}, __osc_ga_fac2)
        __osc_ga_helped1 = false
        __osc_ga_helped2 = false
        function __osc_ga_sample()
            local f1 = __osc_ga_eng1:GetFocusUnit()
            if f1 and f1:GetUnitId() == 'uel0201' then __osc_ga_helped1 = true end
            local f2 = __osc_ga_eng2:GetFocusUnit()
            if f2 and f2:GetUnitId() == 'uel0201' then __osc_ga_helped2 = true end
        end
    )",
                             sx - 50, sz, sx + 10, sz - 12));

    const auto built =
        tick_until(ctx,
                   "Test 1: the engineer helps the factory's tank, then builds "
                   "the power generator queued behind its guard",
                   900, [&] {
                       run_lua(ctx, "__osc_ga_sample()");
                       return lua_true(ctx.lua_state, "__osc_ga_helped1 and "
                                                      "__osc_ga_eng1:GetFocusUnit() and "
                                                      "__osc_ga_eng1:GetFocusUnit():GetUnitId() "
                                                      "== 'ueb1101'");
                   });
    t.check(built.has_value() && !guarding(eng1),
            "Test 1: the engineer's guard ended with the tank, and it builds");
    t.check(lua_true(ctx.lua_state, "__osc_ga_helped2") && guarding(eng2),
            "Test 2: with nothing queued, the other engineer helped its tank and still guards");
    const auto done = tick_until(ctx, "Test 3: the left power generator is finished", 900,
                                 "__osc_ga_pgen:GetFractionComplete() >= 1");
    tick_for(ctx, 2);
    const sim::Unit* e3 = unit(eng3);
    t.check(done.has_value() && e3 && e3->command_queue().empty(),
            "Test 3: an engineer guarding a structure left unfinished finishes it, and its guard "
            "ends with nothing queued");
    run_lua(ctx, fmt::format(R"(
        IssueBuildMobile({{__osc_ga_ward4}}, {{{0}, GetSurfaceHeight({0}, {1}), {1}}}, 'ueb1101', {{}})
        IssueGuard({{__osc_ga_eng4}}, __osc_ga_ward4)
        IssueMove({{__osc_ga_eng4}}, {{{2}, 0, {1}}})
    )",
                             sx, sz + 30, sx - 30));
    const auto first = tick_until(
        ctx, "Test 4: an engineer guarding an engineer on its way to a build builds it first", 600,
        [&] {
            const sim::Unit* g = unit(eng4);
            const sim::Unit* w = unit(ward4);
            return g && w && g->is_building() && !w->is_building();
        });
    const auto moved = tick_until(ctx, "Test 4: it moves on once the building is done", 900, [&] {
        const sim::Unit* g = unit(eng4);
        return g && !g->command_queue().empty() &&
               g->command_queue().front().type == sim::CommandType::Move;
    });
    t.check(first.has_value() && moved.has_value(),
            "Test 4: an engineer guarding an engineer on its way to a build goes and builds it "
            "first, then moves on with its queued order");
    spdlog::info("=== GUARD ABORT TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
