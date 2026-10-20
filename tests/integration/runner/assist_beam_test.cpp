// --assist-beam-test: a commander that began to assist a factory's upgrade partway
// lets its build effects go when the help ends, not at its next build (retail's
// OnStopBuild destroys BuildEffectsBag).

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

void test_assist_beam(TestContext& ctx) {
    spdlog::info("=== ASSIST BEAM TEST: a commander's build effects end with its assist ===");
    Tally t;
    const auto run = [&](const std::string& what, const std::string& code) {
        const auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r), r ? what : fmt::format("{}: {}", what, r.error().message));
        return static_cast<bool>(r);
    };
    const auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            ctx.sim.tick();
        }
    };
    if (!run("a commander beside a land factory, resources to spare", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 100000)
        brain:GiveResource('MASS', 100000)
        brain:GiveResource('ENERGY', 100000)
        local h = GetTerrainHeight
        __osc_ab_acu = CreateUnitHPR('uel0001', 'ARMY_1', 258, h(258, 785), 785, 0, 0, 0)
        __osc_ab_fac = CreateUnitHPR('ueb0101', 'ARMY_1', 262, h(262, 780), 780, 0, 0, 0)
        __osc_ab_heard = {}
        for _, name in {'OnStartBuild', 'OnStopBuild'} do
            local n, f = name, __osc_ab_acu[name]
            __osc_ab_acu[n] = function(self, a, b)
                table.insert(__osc_ab_heard, n)
                return f(self, a, b)
            end
        end
        function __osc_ab_effects()
            local n = 0
            for _ in __osc_ab_acu.BuildEffectsBag do n = n + 1 end
            return n
        end
        function __osc_ab_expect(heard, effects)
            local got = table.concat(__osc_ab_heard, ' ')
            if got ~= heard then error('it heard "' .. got .. '", not "' .. heard .. '"') end
            if __osc_ab_effects() ~= effects then
                error('its build effects number ' .. __osc_ab_effects() .. ', not ' .. effects)
            end
        end
        IssueUpgrade({__osc_ab_fac}, 'ueb0201')
    )")) {
        return;
    }
    ticks(60);
    run("it guards the upgrading factory", R"(
        IssueGuard({__osc_ab_acu}, __osc_ab_fac)
    )");
    ticks(1500);
    run("1: the upgrade done, the help is over: the stop was heard, the effects are gone",
        "__osc_ab_expect('OnStartBuild OnStopBuild', 0)");
    run("a Move", R"(
        IssueMove({__osc_ab_acu}, {258, GetTerrainHeight(258, 800), 800})
    )");
    ticks(2);
    run("2: a Move leaves it no effects of the old help",
        "__osc_ab_expect('OnStartBuild OnStopBuild', 0)");
    run("a Build", R"(
        IssueBuildMobile({__osc_ab_acu}, {250, GetTerrainHeight(250, 790), 790}, 'ueb1101', {})
    )");
    ticks(150);
    run("3: building, its effects are the new build's alone", R"(
        local heard = table.concat(__osc_ab_heard, ' ')
        if heard ~= 'OnStartBuild OnStopBuild OnStartBuild' then error('it heard ' .. heard) end
        if __osc_ab_effects() == 0 then error('no build effects while it builds') end
    )");
    spdlog::info("Assist beam test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
