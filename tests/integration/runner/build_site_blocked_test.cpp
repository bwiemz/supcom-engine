// --build-site-blocked-test: units standing on a build site hold the build up,
// as Moho's mobile build task does (faf-re CUnitMobileBuildTask::Execute,
// TASKSTATE_Processing; Steam 0x005FE4DB). Before it makes its frame the task
// asks whether any unit stands on the site's cells (Sim::LocationIsFree). If
// one does, the army's own resting units are sent off it (SIM_TryToBuild),
// with no order, and the task tries again 49 ticks later (it returns 50),
// failing once it has tried 11 times.
//
// Three scenes, each an engineer in reach of its site:
// 1. three of its army's idle tanks parked on a land factory's site: they go,
//    and the frame stands once they have;
// 2. another army's tank on a power generator's site: it stays, and the build
//    waits until the tank is taken away, then starts;
// 3. another army's tank that never goes: the build fails (OnFailedToBuild)
//    on the 12th look, 11 waits of 49 ticks after the first.
// And a factory (CFactoryBuildTask, Steam 0x00601357), which asks the same of
// its own footprint, but for idle units alone, and never gives up:
// 4. its army's idle tank parked on it is sent off, and then it builds;
// 5. the units it makes, rolled off, don't hold the next one up.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "sim/build_site_units.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace osc::test {

void test_build_site_blocked(TestContext& ctx) {
    spdlog::info("=== BUILD SITE BLOCKED TEST: units on a build site hold the build up ===");
    Tally t;
    const auto run = [&](const std::string& what, const std::string& code) {
        const auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r), r ? what : fmt::format("{}: {}", what, r.error().message));
        return static_cast<bool>(r);
    };
    if (!run("engineers in reach of their sites, tanks on the sites, resources to spare", R"(
        local near = GetUnitsInRect(240, 770, 330, 810)
        if near and table.getn(near) > 0 then error('units near the sites') end
        for _, name in {'ARMY_1', 'ARMY_2'} do
            local brain = GetArmyBrain(name)
            brain:GiveStorage('MASS', 100000)
            brain:GiveResource('MASS', 100000)
            brain:GiveResource('ENERGY', 100000)
        end
        local function make(bp, army, x, z)
            return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, 0, 0)
        end
        local function tank(army, x, z)
            local u = make('uel0201', army, x, z)
            u:SetFireState(1) -- hold fire: no fight between the scenes
            return u
        end
        local function listen(e)
            local heard = {}
            for _, name in {'OnStartBuild', 'OnFailedToBuild'} do
                local n, f = name, e[name]
                e[n] = function(self, a, b)
                    table.insert(heard, n)
                    return f(self, a, b)
                end
            end
            return heard
        end
        -- 1: a land factory's site, its footprint the cells 280-285 by 788-793
        __osc_blk_own = make('uel0105', 'ARMY_1', 282.5, 800)
        __osc_blk_tanks = { tank('ARMY_1', 281, 789.5), tank('ARMY_1', 283, 791.5),
                            tank('ARMY_1', 284.5, 789) }
        __osc_blk_from = {}
        for i, u in __osc_blk_tanks do
            local p = u:GetPosition()
            __osc_blk_from[i] = {p[1], p[3]}
        end
        -- 2 and 3: power generators' sites, the cells (262, 784) and (318, 784)
        __osc_blk_wait = make('uel0105', 'ARMY_1', 260, 790)
        __osc_blk_foe = tank('ARMY_2', 262.5, 784.5)
        __osc_blk_never = make('uel0105', 'ARMY_1', 316, 790)
        __osc_blk_never_heard = listen(__osc_blk_never)
        __osc_blk_stays = tank('ARMY_2', 318.5, 784.5)
        function __osc_blk_on(bp, x, z, r)
            local found = {}
            for _, u in GetUnitsInRect(x - r, z - r, x + r, z + r) or {} do
                if u:GetBlueprint().BlueprintId == bp then
                    table.insert(found, u)
                end
            end
            return found
        end
        -- Whether `u` stands, by its collision box, on the cells x0-x1 by z0-z1.
        function __osc_blk_over(u, x0, z0, x1, z1)
            local p = u:GetPosition()
            local bp = u:GetBlueprint()
            local hx, hz = bp.SizeX * 0.5, bp.SizeZ * 0.5
            local r = math.max(hx, hz) * 1.42
            return p[1] + r > x0 and p[1] - r < x1 and p[3] + r > z0 and p[3] - r < z1
        end
    )")) {
        return;
    }
    tick_for(ctx, 5);
    const u32 ordered = ctx.sim.tick_count();
    run("each engineer ordered to its site", R"(
        IssueBuildMobile({__osc_blk_own}, {282.5, GetTerrainHeight(282.5, 790.5), 790.5}, 'ueb0101', {})
        IssueBuildMobile({__osc_blk_wait}, {262.5, GetTerrainHeight(262.5, 784.5), 784.5}, 'ueb1101', {})
        IssueBuildMobile({__osc_blk_never}, {318.5, GetTerrainHeight(318.5, 784.5), 784.5}, 'ueb1101', {})
    )");

    // 1: the tanks go, with no orders, and then the frame stands.
    const auto own = tick_until(ctx, "the land factory's frame stands", 200,
                                "__osc_blk_own:GetFocusUnit() ~= nil");
    run("1: the frame came only once the tanks had gone, by no order, off its site", R"(
        if table.getn(__osc_blk_on('ueb0101', 282.5, 790.5, 1)) ~= 1 then error('no frame') end
        for i, u in __osc_blk_tanks do
            if __osc_blk_over(u, 280, 788, 285, 793) then
                local p = u:GetPosition()
                error(string.format('tank %d is still on the site, at (%.2f, %.2f)', i, p[1], p[3]))
            end
            local p, was = u:GetPosition(), __osc_blk_from[i]
            if math.abs(p[1] - was[1]) + math.abs(p[3] - was[2]) < 1 then
                error('tank ' .. i .. ' did not move')
            end
            if table.getn(u:GetCommandQueue()) ~= 0 then error('tank ' .. i .. ' was given an order') end
        end
    )");
    t.check(own && *own > 49,
            fmt::format("1: the frame waited out a try, {} ticks", own ? *own : -1));

    // 2: another army's tank stays, and the build waits for it.
    run("2: another army's tank is not sent off, and no frame stands under it", R"(
        if __osc_blk_wait:GetFocusUnit() then error('a frame under the tank') end
        if table.getn(__osc_blk_wait:GetCommandQueue()) ~= 1 then error('the order went') end
        local p = __osc_blk_foe:GetPosition()
        if math.abs(p[1] - 262.5) + math.abs(p[3] - 784.5) > 0.01 then
            error(string.format('the tank moved, to (%.2f, %.2f)', p[1], p[3]))
        end
        if table.getn(__osc_blk_foe:GetCommandQueue()) ~= 0 then error('the tank was given an order') end
        Warp(__osc_blk_foe, {262.5, GetTerrainHeight(262.5, 770), 770})
    )");
    tick_until(ctx, "the power generator's frame stands once the tank is taken off", 50,
               "__osc_blk_wait:GetFocusUnit() ~= nil");

    // 3: a site that never frees: 11 tries, 49 ticks apart, then the order fails.
    const auto never = tick_until(ctx, "the build on the site that never frees fails", 700,
                                  "table.getn(__osc_blk_never:GetCommandQueue()) == 0");
    run("3: the build that never had its site made nothing, and heard it failed", R"(
        if table.getn(__osc_blk_on('ueb1101', 318.5, 784.5, 1)) ~= 0 then error('a frame stands') end
        if table.concat(__osc_blk_never_heard, ' ') ~= 'OnFailedToBuild' then
            error('it heard ' .. table.concat(__osc_blk_never_heard, ' '))
        end
    )");
    if (never) {
        // Its first look comes a few ticks after the order, as it turns to the site.
        const u32 since_order = ctx.sim.tick_count() - ordered;
        t.check(since_order >= 11 * 49 && since_order <= 11 * 49 + 30,
                fmt::format("3: it failed {} ticks after its order (11 waits of 49: 539)",
                            since_order));
    }

    // 4 and 5: a factory looks at its own footprint (CFactoryBuildTask), for idle units alone.
    if (!run("4 and 5: two land factories, one with its army's idle tank on its footprint", R"(
        local near = GetUnitsInRect(250, 815, 320, 850)
        if near and table.getn(near) > 0 then error('units near the factories') end
        local function make(bp, x, z)
            return CreateUnitHPR(bp, 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
        end
        __osc_blk_pad = make('ueb0101', 262.5, 830.5)
        __osc_blk_parked = make('uel0201', 263, 830.5)
        __osc_blk_parked:SetFireState(1)
        __osc_blk_line = make('ueb0101', 302.5, 830.5)
    )")) {
        return;
    }
    tick_for(ctx, 2);
    run("each factory ordered to build Strikers, the second three", R"(
        IssueBuildFactory({__osc_blk_pad}, 'uel0201', 1)
        IssueBuildFactory({__osc_blk_line}, 'uel0201', 3)
    )");
    const auto pad = tick_until(ctx, "the factory with a tank on its footprint builds", 200,
                                "__osc_blk_pad:GetFocusUnit() ~= nil");
    t.check(pad && *pad >= 49,
            fmt::format("4: it waited out a look first, {} ticks", pad ? *pad : -1));
    run("4: the tank went off its footprint, by no order", R"(
        if __osc_blk_over(__osc_blk_parked, 260, 828, 265, 833) then
            local p = __osc_blk_parked:GetPosition()
            error(string.format('the tank is still on it, at (%.2f, %.2f)', p[1], p[3]))
        end
        if table.getn(__osc_blk_parked:GetCommandQueue()) ~= 0 then error('it was given an order') end
    )");
    // 5: the units it makes, rolled off it, don't hold the next up: no wait of a look between.
    int idle = 0;
    int longest = 0;
    bool built_one = false;
    const auto line_done = tick_until(ctx, "the factory makes its three Strikers", 2000, [&] {
        if (lua_true(ctx.lua_state, "__osc_blk_line:IsUnitState('Building')")) {
            built_one = true;
            idle = 0;
        } else if (built_one) {
            longest = std::max(longest, ++idle);
        }
        return lua_true(ctx.lua_state, "table.getn(__osc_blk_line:GetCommandQueue()) == 0");
    });
    t.check(line_done && longest < sim::kSiteRetryTicks,
            fmt::format("5: its longest pause between Strikers was {} ticks", longest));
    spdlog::info("Build site blocked test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
