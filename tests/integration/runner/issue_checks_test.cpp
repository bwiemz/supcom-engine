// --issue-checks-test: the orders a unit takes, as Moho's ProcessIssuedUnitCommand
// takes them (faf-re sim/Sim.cpp), on the cases of the retail probe issue-checks.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <string>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

std::string lua_string(lua::LuaState& state, const std::string& expr) {
    auto r = state.do_string("return tostring(" + expr + ")");
    if (!r) {
        return "error: " + r.error().message;
    }
    lua_State* L = state.raw();
    std::string value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return value;
}

bool run(TestContext& ctx, const char* what, const char* code) {
    if (auto r = ctx.lua_state.do_string(code); !r) {
        test_status::fail("[FAIL] issue-checks-test {}: {}", what, r.error().message);
        return false;
    }
    return true;
}

void check_all(TestContext& ctx, Tally& t, const char* what,
               const std::vector<std::pair<const char*, const char*>>& cases) {
    for (const auto& [expr, expected] : cases) {
        const std::string got = lua_string(ctx.lua_state, expr);
        t.check(got == expected,
                fmt::format("{}: {} gives {} (expected {})", what, expr, got, expected));
    }
}

} // namespace

void test_issue_checks(TestContext& ctx) {
    spdlog::info("=== ISSUE CHECKS TEST: the orders a unit takes ===");
    Tally t;
    if (!run(ctx, "setup", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveResource('MASS', 100000)
        brain:GiveResource('ENERGY', 1000000)
        __ic_acu = brain:GetListOfUnits(categories.COMMAND, false)[1]
        local p = __ic_acu:GetPosition()
        __ic_p = p
        function __ic_make(bp, dx, dz, army)
            local x, z = p[1] + dx, p[3] + dz
            return CreateUnitHPR(bp, army or 'ARMY_1', x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        end
        function __ic_n(u)
            return table.getn(u:GetCommandQueue() or {})
        end
        function __ic_try(u, f)
            IssueClearCommands({u})
            f()
            local k = __ic_n(u)
            IssueClearCommands({u})
            return k
        end
        __ic_pt = {p[1] + 12, 0, p[3] - 12}
        __ic_friend = __ic_make('uel0201', 20, 0)
        __ic_friend:SetHealth(nil, __ic_friend:GetMaxHealth() / 2)
        __ic_friend2 = __ic_make('uel0201', 24, 0)
        __ic_enemy = __ic_make('ueb1101', 0, 80, 'ARMY_2')
        function __ic_orders(u)
            local s = 'move=' .. __ic_try(u, function() IssueMove({u}, __ic_pt) end)
            s = s .. ' guard=' .. __ic_try(u, function() IssueGuard({u}, __ic_friend) end)
            if EntityCategoryContains(categories.RECLAIM, u) then
                s = s .. ' reclaim=' .. __ic_try(u, function() IssueReclaim({u}, __ic_friend2) end)
                s = s .. ' repair=' .. __ic_try(u, function() IssueRepair({u}, __ic_friend) end)
                s = s .. ' capture=' .. __ic_try(u, function() IssueCapture({u}, __ic_enemy) end)
            end
            return s
        end

        __ic_tr = __ic_make('uea0104', -20, 30)
        __ic_ct = __ic_make('uel0201', -24, 34)
        __ic_ce = __ic_make('uel0105', -16, 34)
        IssueTransportLoad({__ic_ct, __ic_ce}, __ic_tr)
        __ic_cz = __ic_make('uaa0310', 60, 60)
        __ic_cpl = __ic_make('uea0102', 56, 40)
        __ic_cpod = __ic_make('uea0001', 60, 40)
        __ic_ceng = __ic_make('uel0105', 64, 40)
        for _, u in {__ic_cpl, __ic_cpod, __ic_ceng} do
            __ic_cz:AddUnitToStorage(u)
        end
    )")) {
        return;
    }
    tick_until(ctx, "issue-checks-test: the transport loads", 600,
               "__ic_ct:IsUnitState('Attached') and __ic_ce:IsUnitState('Attached')");
    check_all(ctx, t, "a carried unit's orders",
              {
                  {"__ic_orders(__ic_ct)", "move=0 guard=0"},
                  {"__ic_orders(__ic_ce)", "move=0 guard=0 reclaim=0 repair=0 capture=0"},
                  {"__ic_orders(__ic_cpl)", "move=1 guard=1"},
                  {"__ic_orders(__ic_cpod)", "move=1 guard=1 reclaim=1 repair=1 capture=1"},
                  {"__ic_orders(__ic_ceng)", "move=1 guard=1 reclaim=0 repair=0 capture=0"},
                  {"__ic_orders(__ic_make('uel0201', 40, 10))", "move=1 guard=1"},
                  {"__ic_orders(__ic_make('uel0105', 44, 10))",
                   "move=1 guard=1 reclaim=1 repair=1 capture=1"},
                  {"__ic_orders(__ic_make('uea0001', 48, 10))",
                   "move=1 guard=1 reclaim=1 repair=1 capture=1"},
              });
    spdlog::info("issue-checks-test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
