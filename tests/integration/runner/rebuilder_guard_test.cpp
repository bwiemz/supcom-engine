// --rebuilder-guard-test: a REBUILDER's guard, as the original game plays it
// (tools/moho_probes `rebuild` and `scu-assist`, 2026-10-13), each beside an
// engineer's as the control.
//
// A support commander (UEL0301, REBUILDER) guards a T1 power generator, and a
// T1 engineer guards another. The support commander's is damaged, and it
// repairs it; then both generators are destroyed.
// 1. The engineer's Guard ends with its generator; the support commander's
//    stays, and its repair stops.
// 2. The support commander goes to where its generator stood.
// A support commander and an engineer guard a T1 land factory building tanks.
// 3. The engineer builds with it; the support commander never does (Moho's
//    guard task takes up no live unit a REBUILDER guards).
//
// Before, the support commander's Guard ended as the engineer's did, and it
// built with the factory.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

void test_rebuilder_guard(TestContext& ctx) {
    spdlog::info("=== REBUILDER GUARD TEST: a support commander guards a place ===");
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
    // Run `code`, a check that errors with what went wrong.
    const auto lua_check = [&](const std::string& what, const std::string& code) {
        const auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r), r ? what : fmt::format("{}: {}", what, r.error().message));
    };

    spawn_unit(ctx, "__osc_rb_pgen", "ueb1101", "ARMY_1", {sx, sz});
    const u32 scu_id = spawn_unit(ctx, "__osc_rb_scu", "uel0301", "ARMY_1", {sx, sz + 12});
    spawn_unit(ctx, "__osc_rb_pgen2", "ueb1101", "ARMY_1", {sx - 20, sz});
    const u32 eng_id = spawn_unit(ctx, "__osc_rb_eng", "uel0105", "ARMY_1", {sx - 20, sz + 8});
    spawn_unit(ctx, "__osc_rb_factory", "ueb0101", "ARMY_1", {sx + 30, sz});
    spawn_unit(ctx, "__osc_rb_scu2", "uel0301", "ARMY_1", {sx + 30, sz + 18});
    spawn_unit(ctx, "__osc_rb_eng2", "uel0105", "ARMY_1", {sx + 36, sz + 18});
    if (!unit(scu_id) || !unit(eng_id)) {
        t.check(false, "the units spawn");
        return;
    }
    run_lua(ctx, R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 50000)
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('MASS', 50000)
        brain:GiveResource('ENERGY', 500000)
        IssueGuard({__osc_rb_scu}, __osc_rb_pgen)
        IssueGuard({__osc_rb_eng}, __osc_rb_pgen2)
        IssueBuildFactory({__osc_rb_factory}, 'uel0201', 10)
        IssueGuard({__osc_rb_scu2}, __osc_rb_factory)
        IssueGuard({__osc_rb_eng2}, __osc_rb_factory)
        __osc_rb_site = __osc_rb_pgen:GetPosition()
    )");
    for (int i = 0; i < 60; ++i) ctx.sim.tick();
    run_lua(ctx, "__osc_rb_pgen:SetHealth(nil, __osc_rb_pgen:GetMaxHealth() * 0.5)");
    bool repaired = false;
    for (int i = 0; i < 60 && !repaired; ++i) {
        ctx.sim.tick();
        repaired = unit(scu_id) && unit(scu_id)->is_repairing();
    }
    t.check(repaired, "setup: the support commander repairs its damaged generator");
    const sim::Unit* before = unit(scu_id);
    const f32 off_before =
        before ? std::hypot(before->position().x - sx, before->position().z - sz) : 0.0f;
    run_lua(ctx, "__osc_rb_pgen:Kill() __osc_rb_pgen2:Kill()");
    for (int i = 0; i < 30; ++i) ctx.sim.tick();

    const auto guards = [&](u32 id) {
        const sim::Unit* u = unit(id);
        return u && !u->command_queue().empty() &&
               u->command_queue().front().type == sim::CommandType::Guard;
    };
    t.check(guards(scu_id) && unit(eng_id) && unit(eng_id)->command_queue().empty() &&
                !unit(scu_id)->is_repairing(),
            "Test 1: the engineer's Guard ends with its generator; the support commander's "
            "stays, and its repair stops");

    // Over the next ticks: where it goes, and what each factory guard works on.
    run_lua(ctx, R"(
        __osc_rb_eng_helped = false
        __osc_rb_scu_helped = false
        function __osc_rb_focus(u)
            local f = u:GetFocusUnit()
            return f and f:GetUnitId() or 'none'
        end
        function __osc_rb_sample()
            if __osc_rb_focus(__osc_rb_eng2) == 'uel0201' then __osc_rb_eng_helped = true end
            if __osc_rb_focus(__osc_rb_scu2) ~= 'none' then __osc_rb_scu_helped = true end
        end
    )");
    for (int i = 0; i < 120; ++i) {
        ctx.sim.tick();
        run_lua(ctx, "__osc_rb_sample()");
    }
    const sim::Unit* scu = unit(scu_id);
    const f32 off = scu ? std::hypot(scu->position().x - sx, scu->position().z - sz) : 1e9f;
    t.check(scu && guards(scu_id) && off_before > 5.0f && off < 3.0f,
            fmt::format("Test 2: it goes to where its generator stood ({:.1f} off, from {:.1f})",
                        off, off_before));

    lua_check("Test 3: the engineer builds with the factory; the support commander never does",
              R"(
        if not __osc_rb_eng_helped then error('the engineer never built with it') end
        if __osc_rb_scu_helped then error('the support commander built with it') end
    )");
    spdlog::info("=== REBUILDER GUARD TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
