// --mobile-build-start-test: a T1 engineer starting a build, tick by tick, as
// the original game plays it (tools/moho_probes `engineer`, 2026-10-14; two
// runs, identical).
//
// A T1 engineer (UEL0105) at (260, 790), heading 0, is told to build a T1
// power generator at (262, 784), behind it, with resources to spare. From
// the order, the original:
// 1. puts the site on its footprint's cell: (262.5, 784.5) (Moho's build
//    task snaps it as it is made);
// 2. aims a tick after the order (its task's Preparing tick), so
//    OnPrepareArmToBuild runs on tick 1 and the engineer is Building from 2;
// 3. opens its arm (uel0105_aopen at rate 5) from the next tick, so the
//    arm is enabled on tick 6;
// 4. turns the arm 36 degrees a tick (SetAimingArc's 360 a second), from
//    the root bone's line to the site, and makes the frame on tick 9, the
//    arm 11 degrees off (inside 15); the arm settles on tick 10.
// 5. built, closes its arm (SetRate(-5)) on its way to the next build, and
//    opening it there waits as long as the first time.
// 6. a build right after another in its reach begins a tick after its arm is
//    aimed: the finished build's aim was cleared as it ended.
// Before, the engine built at the order's point, aimed on the order's tick,
// opened the arm a tick sooner and turned it in one tick: the frame came on
// tick 5.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace osc::test {

namespace {

/// The original's row for tick `t` after the order: Building, the frame,
/// and the build turret's direction (x, z).
struct Row {
    bool building;
    bool frame;
    f32 turret_x, turret_z;
};

constexpr std::array<Row, 13> kOriginal = {{
    {false, false, 0.000f, 1.000f}, // 0
    {false, false, 0.000f, 1.000f}, // 1
    {true, false, 0.000f, 1.000f},  // 2
    {true, false, 0.000f, 1.000f},  // 3
    {true, false, 0.000f, 1.000f},  // 4
    {true, false, 0.000f, 1.000f},  // 5
    {true, false, 0.000f, 1.000f},  // 6
    {true, false, 0.588f, 0.809f},  // 7
    {true, false, 0.951f, 0.309f},  // 8
    {true, false, 0.951f, -0.309f}, // 9
    {true, true, 0.588f, -0.809f},  // 10
    {true, true, 0.414f, -0.910f},  // 11
    {true, true, 0.414f, -0.910f},  // 12
}};

/// The tick the original ran each callback on, as the row the probe logged
/// just before it.
struct Event {
    const char* name;
    int row;
};
constexpr std::array<Event, 4> kOriginalEvents = {{
    {"OnPrepareArmToBuild", 1},
    {"WaitForBuildAnimation-done", 6},
    {"OnStartBuild", 9},
    {"OnStopBuilderTracking", 11},
}};

f64 global_number(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -999.0;
    lua_pop(L, 1);
    return v;
}

} // namespace

void test_mobile_build_start(TestContext& ctx) {
    spdlog::info("=== MOBILE BUILD START TEST: an engineer starts a build, as the original ===");
    Tally t;
    lua_State* L = ctx.L;
    const auto run = [&](const std::string& what, const std::string& code) {
        const auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r), r ? what : fmt::format("{}: {}", what, r.error().message));
        return static_cast<bool>(r);
    };
    if (!run("the engineer, its site clear, resources to spare",
             "local near = GetUnitsInRect(248, 778, 274, 802)\n"
             "if near and table.getn(near) > 0 then error('units near the site') end\n"
             "local brain = GetArmyBrain('ARMY_1')\n"
             "brain:GiveStorage('MASS', 100000)\n"
             "brain:GiveResource('MASS', 100000)\n"
             "brain:GiveResource('ENERGY', 100000)\n"
             "__osc_mb_eng = CreateUnitHPR('uel0105', 'ARMY_1', 260, GetTerrainHeight(260, 790), "
             "790, 0, 0, 0)\n")) {
        return;
    }
    for (int i = 0; i < 5; ++i) {
        ctx.sim.tick();
    }
    // Each callback, noted with the row it falls after; the rows, logged by a thread as the
    // original's probe logged them
    (void)ctx.lua_state.do_string(fmt::format("__osc_mb_last = {}\n", kOriginal.size() - 1));
    if (!run("the engineer's build callbacks watched, and the order given",
             "__osc_mb_row = -1\n"
             "__osc_mb_events = {}\n"
             "__osc_mb_rows = {}\n"
             "local eng = __osc_mb_eng\n"
             "for _, name in {'OnPrepareArmToBuild', 'WaitForBuildAnimation', 'OnStartBuild',\n"
             "                'OnStopBuilderTracking'} do\n"
             "    local old, n = eng[name], name\n"
             "    eng[n] = function(self, a, b, c)\n"
             "        local r = old(self, a, b, c)\n"
             "        local key = n == 'WaitForBuildAnimation' and n .. '-done' or n\n"
             "        if not __osc_mb_events[key] then __osc_mb_events[key] = __osc_mb_row end\n"
             "        return r\n"
             "    end\n"
             "end\n"
             "ForkThread(function()\n"
             "    IssueBuildMobile({eng}, {262, GetTerrainHeight(262, 784), 784}, 'ueb1101', {})\n"
             "    for row = 0, __osc_mb_last do\n"
             "        __osc_mb_row = row\n"
             "        local tx, _, tz = eng:GetBoneDirection('Turret')\n"
             "        __osc_mb_rows[row + 1] = {eng:IsUnitState('Building') and 1 or 0,\n"
             "                                  eng:GetFocusUnit() and 1 or 0, tx, tz}\n"
             "        WaitTicks(1)\n"
             "    end\n"
             "end)\n")) {
        return;
    }
    for (size_t i = 0; i < kOriginal.size() + 1; ++i) {
        ctx.sim.tick();
    }
    int matched = 0;
    for (int row = 0; row < static_cast<int>(kOriginal.size()); ++row) {
        const auto sampled = ctx.lua_state.do_string(
            fmt::format("local r = __osc_mb_rows[{}] or {{-1, -1, 0, 0}}\n"
                        "__osc_mb_building, __osc_mb_frame, __osc_mb_tx, __osc_mb_tz = "
                        "r[1], r[2], r[3], r[4]\n",
                        row + 1));
        if (!sampled) {
            t.check(false, fmt::format("row {} read: {}", row, sampled.error().message));
            return;
        }
        const Row& want = kOriginal[static_cast<size_t>(row)];
        const bool building = global_number(L, "__osc_mb_building") == 1.0;
        const bool frame = global_number(L, "__osc_mb_frame") == 1.0;
        const auto tx = static_cast<f32>(global_number(L, "__osc_mb_tx"));
        const auto tz = static_cast<f32>(global_number(L, "__osc_mb_tz"));
        const bool same = building == want.building && frame == want.frame &&
                          std::abs(tx - want.turret_x) < 0.002f &&
                          std::abs(tz - want.turret_z) < 0.002f;
        if (same) {
            ++matched;
        } else {
            spdlog::warn("  t={}: Building {} frame {} turret {:.3f},{:.3f}; the original: {} {} "
                         "{:.3f},{:.3f}",
                         row, building, frame, tx, tz, want.building, want.frame, want.turret_x,
                         want.turret_z);
        }
    }
    t.check(matched == static_cast<int>(kOriginal.size()),
            fmt::format("1-4: {} of {} ticks as the original's (Building, the frame, the build "
                        "turret's direction)",
                        matched, kOriginal.size()));

    for (const Event& e : kOriginalEvents) {
        (void)ctx.lua_state.do_string(
            fmt::format("__osc_mb_event = __osc_mb_events['{}'] or -1\n", e.name));
        const auto at = static_cast<int>(global_number(L, "__osc_mb_event"));
        t.check(at == e.row, fmt::format("2-4: {} on the tick after row {} (the original's {})",
                                         e.name, at, e.row));
    }

    run("the frame's place read", "local p = __osc_mb_eng:GetFocusUnit():GetPosition()\n"
                                  "__osc_mb_x, __osc_mb_z = p[1], p[3]\n");
    const f64 x = global_number(L, "__osc_mb_x");
    const f64 z = global_number(L, "__osc_mb_z");
    t.check(x == 262.5 && z == 784.5,
            fmt::format("1: the frame on its footprint's cell, ({}, {}) as the original's "
                        "(262.5, 784.5)",
                        x, z));

    if (!run("5: the next build queued, out of the engineer's reach",
             "__osc_mb_first = __osc_mb_eng:GetFocusUnit()\n"
             "IssueBuildMobile({__osc_mb_eng}, {272, GetTerrainHeight(272, 800), 800}, 'ueb1101', "
             "{})\n")) {
        return;
    }
    tick_until(ctx, "5: the power generator is built", 600,
               "__osc_mb_first:GetFractionComplete() == 1");
    if (!run("5: the second build's callbacks watched", "__osc_mb_events = {}\n"
                                                        "__osc_mb_row = -1\n"
                                                        "ForkThread(function()\n"
                                                        "    for row = 0, 400 do\n"
                                                        "        __osc_mb_row = row\n"
                                                        "        WaitTicks(1)\n"
                                                        "    end\n"
                                                        "end)\n")) {
        return;
    }
    tick_until(ctx, "5: the arm closes on the way", 60,
               "__osc_mb_eng.BuildingOpenAnimManip:GetAnimationFraction() == 0 and "
               "__osc_mb_events['OnPrepareArmToBuild'] == nil");
    tick_until(ctx, "5: the arm opens at the next site", 300,
               "__osc_mb_events['WaitForBuildAnimation-done'] ~= nil");
    (void)ctx.lua_state.do_string(
        "__osc_mb_prepare = __osc_mb_events['OnPrepareArmToBuild'] or -1\n"
        "__osc_mb_open = __osc_mb_events['WaitForBuildAnimation-done'] or -1\n");
    const auto prepare = static_cast<int>(global_number(L, "__osc_mb_prepare"));
    const auto open = static_cast<int>(global_number(L, "__osc_mb_open"));
    t.check(prepare >= 0 && open - prepare == kOriginalEvents[1].row - kOriginalEvents[0].row,
            fmt::format("5: the arm opens {} ticks after OnPrepareArmToBuild (rows {} and {}), "
                        "as on the first build ({})",
                        open - prepare, prepare, open,
                        kOriginalEvents[1].row - kOriginalEvents[0].row));

    if (!run(
            "6: another engineer given two walls straight ahead, in its reach",
            "__osc_mb_b = CreateUnitHPR('uel0105', 'ARMY_1', 300, GetTerrainHeight(300, 790), 790, "
            "0, 0, 0)\n"
            "__osc_mb_b_prepare = {}\n"
            "__osc_mb_b_start = {}\n"
            "for name, ticks in {OnPrepareArmToBuild = __osc_mb_b_prepare,\n"
            "                    OnStartBuild = __osc_mb_b_start} do\n"
            "    local old, list = __osc_mb_b[name], ticks\n"
            "    __osc_mb_b[name] = function(self, a, b)\n"
            "        table.insert(list, GetGameTick())\n"
            "        return old(self, a, b)\n"
            "    end\n"
            "end\n"
            "for _, z in {793.5, 794.5} do\n"
            "    IssueBuildMobile({__osc_mb_b}, {300.5, GetTerrainHeight(300.5, z), z}, 'ueb5101', "
            "{})\n"
            "end\n")) {
        return;
    }
    tick_until(ctx, "6: the second wall is begun", 200, "table.getn(__osc_mb_b_start) >= 2");
    (void)ctx.lua_state.do_string(
        "__osc_mb_b_wait = (__osc_mb_b_start[2] or -1) - (__osc_mb_b_prepare[2] or -1)\n");
    const auto wait = static_cast<int>(global_number(L, "__osc_mb_b_wait"));
    t.check(wait == 1,
            fmt::format("6: the second wall begun {} ticks after its OnPrepareArmToBuild, "
                        "1 as the original's",
                        wait));
    spdlog::info("Mobile build start test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
