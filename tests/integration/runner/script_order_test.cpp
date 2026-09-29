// --script-order-test (M206w): Script orders run retail's own tasks, as
// Moho's CUnitScriptTask runs them.
//
// 1. The UEF ACU enhances (AdvancedEngineering) through IssueScript with
//    retail's EnhanceTask, as retail's AI orders it: the work progresses
//    with the unit Enhancing and Upgrading (what the AI waits on), and
//    ends with the enhancement, the Enhancing/Upgrading states gone, the
//    order done and its AI result Success.
// 2. Another enhancement, cleared midway: the task ends, retail's
//    OnWorkFail runs, no enhancement, AI result Fail.
// 3. The Eye of Rhianne scries a point through retail's TargetLocation (the
//    ability its orders button gives): its OnTargetLocation takes the
//    energy and opens the remote view there.
//
// The engine had turned the construction panel's EnhanceTask into its own
// enhancement order and dropped every other Script order: IssueScript was
// a no-op (retail's AI never enhanced) and the Eye could not scry.

#include "integration_tests.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "renderer/input_handler.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <string>

namespace osc::test {

namespace {

sim::Unit* acu_of(sim::SimState& sim) {
    auto* e = sim.entity_registry().find(army_acu_id(sim, 0));
    return e && e->is_unit() ? static_cast<sim::Unit*>(e) : nullptr;
}

/// Run `code` (Lua); a failure is the test's.
bool run(TestContext& ctx, const char* what, const std::string& code) {
    if (auto r = ctx.lua_state.do_string(code); !r) {
        test_status::fail("[FAIL] script orders: {}: {}", what, r.error().message);
        return false;
    }
    return true;
}

} // namespace

void test_script_orders(TestContext& ctx) {
    spdlog::info("=== Script orders: retail's EnhanceTask and TargetLocation ===");
    if (!run(ctx, "setup", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 50000)
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('MASS', 50000)
        brain:GiveResource('ENERGY', 500000)
        __acu = GetEntityById(__osc_test_acu_id(1))
        if __acu:HasEnhancement('AdvancedEngineering') then error('enhanced already') end
        IssueScript({__acu}, { TaskName = 'EnhanceTask', Enhancement = 'AdvancedEngineering' })
    )"))
        return;

    // 1. It works, and finishes
    f32 midway = 0;
    bool done = false;
    for (int i = 1; i <= 1500 && !done; ++i) {
        ctx.sim.tick();
        auto* acu = acu_of(ctx.sim);
        if (!acu) break;
        if (i == 300) {
            midway = acu->work_progress();
            // What retail's AI waits on (platoon.lua's EnhanceAI)
            run(ctx, "the states midway", R"(
                if not __acu:IsUnitState('Enhancing') then error('not Enhancing') end
                if not __acu:IsUnitState('Upgrading') then error('not Upgrading') end
            )");
        }
        done = acu->command_queue().empty() && !acu->has_script_task();
    }
    auto* acu = acu_of(ctx.sim);
    if (!acu || !done) {
        test_status::fail("[FAIL] script orders: EnhanceTask never finished (progress {:.2f})",
                          acu ? acu->work_progress() : -1.0f);
        return;
    }
    if (midway <= 0.0f || midway >= 1.0f)
        test_status::fail("[FAIL] script orders: EnhanceTask's progress at tick 300: {:.2f}",
                          midway);
    if (acu->script_task_result() != 1)
        test_status::fail("[FAIL] script orders: EnhanceTask's AI result {}, not Success",
                          acu->script_task_result());
    if (run(ctx, "the enhancement", R"(
        if not __acu:HasEnhancement('AdvancedEngineering') then error('not enhanced') end
        if __acu:IsUnitState('Enhancing') or __acu:IsUnitState('Upgrading') then
            error('still enhancing')
        end
        if __acu:GetWorkProgress() ~= 0 then error('progress ' .. __acu:GetWorkProgress()) end
    )"))
        spdlog::info("[PASS] script orders: the ACU enhanced through retail's EnhanceTask "
                     "(progress {:.2f} at tick 300)",
                     midway);

    // 2. Cleared midway: it fails its work
    if (!run(ctx, "the second enhancement",
             "IssueScript({__acu}, { TaskName = 'EnhanceTask', Enhancement = 'Shield' })"))
        return;
    for (int i = 0; i < 100; ++i) ctx.sim.tick();
    acu = acu_of(ctx.sim);
    const bool working = acu && acu->has_script_task() && acu->work_progress() > 0.0f;
    if (!working) test_status::fail("[FAIL] script orders: the Shield enhancement never began");
    if (!run(ctx, "the clear", "IssueClearCommands({__acu})")) return;
    ctx.sim.tick();
    acu = acu_of(ctx.sim);
    if (!acu || acu->has_script_task() || acu->script_task_result() != 2)
        test_status::fail("[FAIL] script orders: the cleared EnhanceTask didn't fail (result {})",
                          acu ? acu->script_task_result() : -1);
    if (working && run(ctx, "the cancel", R"(
        if __acu:HasEnhancement('Shield') then error('enhanced anyway') end
        if __acu:IsUnitState('Enhancing') then error('still enhancing') end
    )"))
        spdlog::info("[PASS] script orders: a cleared EnhanceTask fails its work (OnWorkFail)");

    // 3. The Eye of Rhianne scries
    if (!run(ctx, "the Eye", R"(
        local brain = GetArmyBrain('ARMY_1')
        local pos = __acu:GetPosition()
        __eye = CreateUnitHPR('xab3301', 'ARMY_1', pos[1] + 30, GetTerrainHeight(pos[1] + 30, pos[3]),
                              pos[3], 0, 0, 0)
        if not __eye then error('no Eye of Rhianne') end
        __energy = brain:GetEconomyStored('ENERGY')
        __spot = { pos[1] + 200, 0, pos[3] + 200 }
        __spot[2] = GetTerrainHeight(__spot[1], __spot[3])
        IssueScript({__eye}, { TaskName = 'TargetLocation', Location = __spot })
    )"))
        return;
    for (int i = 0; i < 5; ++i) ctx.sim.tick();
    if (run(ctx, "the scry", R"(
        local data = __eye.RemoteViewingData
        local at = data and data.VisibleLocation
        if not at or at[1] ~= __spot[1] or at[3] ~= __spot[3] then
            error('it looks at ' .. repr(at))
        end
        if not data.Satellite then error('no remote view') end
        local spent = __energy - GetArmyBrain('ARMY_1'):GetEconomyStored('ENERGY')
        if spent < 10000 then error('it took ' .. spent .. ' energy') end
    )"))
        spdlog::info("[PASS] script orders: the Eye of Rhianne scries through TargetLocation");

    // 4. As the orders panel's ability button gives it: a click in its
    // command mode, the click the task's Location. The ACU, selected too,
    // has no ability to take it.
    if (!run(ctx, "the Eye's id", "__eye_id = tonumber(__eye:GetEntityId())")) return;
    lua_getglobal(ctx.L, "__eye_id");
    const auto eye_id = static_cast<u32>(lua_tonumber(ctx.L, -1));
    lua_pop(ctx.L, 1);
    acu = acu_of(ctx.sim);
    if (!acu) return;
    const sim::Vector3 at = acu->position();
    const f32 cx = at.x - 150.0f;
    const f32 cz = at.z + 150.0f;
    renderer::InputHandler input;
    input.set_player_army(0);
    input.set_selected({eye_id, acu->entity_id()});
    renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Script";
    // The mode's table as orders.lua makes it, plus the click (the app's
    // builder reads it from commandmode.lua)
    mode.script_args_at = [&](const sim::Vector3& p) -> std::string {
        lua_State* L = ctx.L;
        const std::string code =
            fmt::format("return {{ name = 'RULEUCC_Script', AbilityName = 'TargetLocation', "
                        "TaskName = 'TargetLocation', Location = {{ {}, {}, {} }} }}",
                        p.x, p.y, p.z);
        if (luaL_loadbuffer(L, code.c_str(), code.size(), "=mode") != 0 ||
            lua_pcall(L, 0, 1, 0) != 0) {
            lua_pop(L, 1);
            return {};
        }
        auto bytes = sim::lua_to_bytes(L, -1);
        lua_pop(L, 1);
        return bytes ? *bytes : std::string();
    };
    const auto issued = input.click_in_command_mode(ctx.sim, mode, cx, cz, false);
    for (int i = 0; i < 5; ++i) ctx.sim.tick();
    acu = acu_of(ctx.sim);
    if (!issued || issued->type != "Script") {
        test_status::fail("[FAIL] script orders: the ability's click issued no Script order");
    } else if (acu && !acu->command_queue().empty()) {
        test_status::fail("[FAIL] script orders: the ACU took the Eye's ability order");
    } else if (run(ctx, "the clicked scry",
                   fmt::format(R"(
        local at = __eye.RemoteViewingData.VisibleLocation
        if math.abs(at[1] - {}) > 0.01 or math.abs(at[3] - {}) > 0.01 then
            error('it looks at ' .. repr(at))
        end
    )",
                               cx, cz))) {
        spdlog::info("[PASS] script orders: the ability's click scries where it clicked");
    }
}

} // namespace osc::test
