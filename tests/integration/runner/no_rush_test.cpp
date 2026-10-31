// --no-rush-test: No Rush on the cases of the retail probes norush-zone, norush-border and
// norush-targets.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "lua/session_manager.hpp"
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

} // namespace

void test_no_rush(TestContext& ctx) {
    spdlog::info("=== NO RUSH TEST: each army's zone, as the original game's ===");
    Tally t;
    ctx.sim.set_no_rush(300.0f, 0.0f);
    lua::apply_no_rush_scenario(ctx.L, ctx.sim);
    if (auto r = ctx.lua_state.do_string(R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveResource('MASS', 100000)
        brain:GiveResource('ENERGY', 1000000)
        local sx, sz = brain:GetArmyStartPos()
        local ox, oz = ScenarioInfo.norushoffsetX_ARMY_1, ScenarioInfo.norushoffsetY_ARMY_1
        local R = ScenarioInfo.norushradius
        local cx, cz = sx + ox, sz + oz
        __nr = {sx = sx, sz = sz, cx = cx, cz = cz, R = R}
        local function make(bp, x, z, army)
            return CreateUnitHPR(bp, army or 'ARMY_1', x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        end
        function __nr.dist(u, x, z)
            local p = u:GetPosition()
            return math.sqrt((p[1] - (x or cx)) ^ 2 + (p[3] - (z or cz)) ^ 2)
        end
        local t = make('uel0201', sx + 4, sz + 4)
        function __nr.move(dx, dz)
            IssueClearCommands({t})
            IssueMove({t}, {sx + dx, 0, sz + dz})
            local n = table.getn(t:GetCommandQueue() or {})
            IssueClearCommands({t})
            return n
        end
        __nr.fOut = make('ueb0101', sx + 12, sz + 12)
        __nr.fIn = make('ueb0101', sx - 12, sz + 12)
        __nr.rallyIn = {cx, 0, cz - R + 25}
        IssueFactoryRallyPoint({__nr.fOut}, {cx, 0, cz - R - 25})
        IssueFactoryRallyPoint({__nr.fIn}, __nr.rallyIn)
        IssueBuildFactory({__nr.fOut}, 'uel0105', 1)
        IssueBuildFactory({__nr.fIn}, 'uel0105', 1)

        __nr.ward = make('uel0105', cx - 10, cz - 30)
        __nr.guard = make('uel0201', cx, cz - 30)
        IssueGuard({__nr.guard}, __nr.ward)

        local function dry(x, z)
            return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) <= 0
        end
        for a = 0, 35 do
            local ux, uz = math.cos(a * math.pi / 18), math.sin(a * math.pi / 18)
            local ok = true
            for r = R - 50, R + 30, 2 do
                ok = ok and dry(cx + ux * r, cz + uz * r)
            end
            if ok then
                __nr.dir = {ux, uz}
                break
            end
        end
        local function along(r)
            return cx + __nr.dir[1] * r, cz + __nr.dir[2] * r
        end
        local x, z = along(R - 6)
        __nr.tankOut = make('uel0201', x, z)
        x, z = along(R + 8)
        __nr.foeOut = make('uel0201', x, z, 'ARMY_2')
        x, z = along(R - 48)
        __nr.tankIn = make('uel0201', x, z)
        x, z = along(R - 34)
        __nr.foeIn = make('uel0201', x, z, 'ARMY_2')
        for _, f in {__nr.foeOut, __nr.foeIn} do
            f:SetFireState(1)
        end
        x, z = along(R + 25)
        __nr.outside = make('uel0201', x, z)
        __nr.outsideAt = __nr.dist(__nr.outside)
    )");
        !r) {
        test_status::fail("[FAIL] no-rush-test setup: {}", r.error().message);
        return;
    }
    spdlog::info("no-rush-test: start {},{} centre {},{} radius {}",
                 lua_string(ctx.lua_state, "__nr.sx"), lua_string(ctx.lua_state, "__nr.sz"),
                 lua_string(ctx.lua_state, "__nr.cx"), lua_string(ctx.lua_state, "__nr.cz"),
                 lua_string(ctx.lua_state, "__nr.R"));
    const auto check_all = [&](const char* what,
                               const std::vector<std::pair<const char*, const char*>>& cases) {
        for (const auto& [expr, expected] : cases) {
            const std::string got = lua_string(ctx.lua_state, expr);
            t.check(got == expected,
                    fmt::format("{}: {} gives {} (expected {})", what, expr, got, expected));
        }
    };
    check_all("the zone", {
                              {"__nr.R", "70"},
                              {"__nr.cx - __nr.sx", "15"},
                              {"__nr.cz - __nr.sz", "40"},
                              {"__nr.move(0, -60)", "0"},
                              {"__nr.move(15, 100)", "1"},
                              {"__nr.move(15 + 68, 40)", "1"},
                              {"__nr.move(15 + 72, 40)", "0"},
                          });
    if (auto r = ctx.lua_state.do_string("Warp(__nr.ward, {__nr.cx - 100, 0, __nr.cz - 30})"); !r) {
        test_status::fail("[FAIL] no-rush-test warp: {}", r.error().message);
        return;
    }
    tick_for(ctx, 400);
    check_all(
        "at the edge and past it",
        {
            {"__nr.dist(__nr.guard) > __nr.R + 10", "true"},
            {"math.abs(__nr.dist(__nr.outside) - __nr.outsideAt) < 0.5", "true"},
            {"__nr.foeIn:IsDead() or __nr.foeIn:GetHealth() < __nr.foeIn:GetMaxHealth()", "true"},
            {"__nr.foeOut:GetHealth() == __nr.foeOut:GetMaxHealth()", "true"},
        });
    check_all(
        "a factory's rally point",
        {
            {"(function() for _, u in GetArmyBrain('ARMY_1'):GetListOfUnits(categories.uel0105, "
             "false) do if u ~= __nr.ward and __nr.dist(u, __nr.fOut:GetPosition()[1], "
             "__nr.fOut:GetPosition()[3]) < 15 then return 'near' end end return 'none' end)()",
             "near"},
            {"(function() for _, u in GetArmyBrain('ARMY_1'):GetListOfUnits(categories.uel0105, "
             "false) do if u ~= __nr.ward and __nr.dist(u, __nr.rallyIn[1], __nr.rallyIn[3]) < 6 "
             "then return 'there' end end return 'none' end)()",
             "there"},
        });
    ctx.sim.set_no_rush(0.0f, 0.0f);
    spdlog::info("no-rush-test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
