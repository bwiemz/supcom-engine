#include "pathing_evidence_test.hpp"

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

namespace {

/// Run `code`, a check that errors with what went wrong.
void lua_check(TestContext& ctx, Tally& t, const char* what, const char* code) {
    const auto r = ctx.lua_state.do_string(code);
    t.check(static_cast<bool>(r),
            r ? std::string(what) : fmt::format("{}: {}", what, r.error().message));
}

/// Tick the sim `ticks` times, calling Lua's `sample(tick)` after each.
void run_sampled(TestContext& ctx, Tally& t, int ticks, const char* sample) {
    for (int i = 1; i <= ticks; ++i) {
        ctx.sim.tick();
        const auto r = ctx.lua_state.do_string(fmt::format("{}({})", sample, i));
        if (!r) {
            t.check(false, fmt::format("sampling at tick {}: {}", i, r.error().message));
            return;
        }
    }
}

/// The map's other armies start with their commanders on it; one near a
/// scene would fight it. Make them neutral to ARMY_1, so none does.
void make_peace(TestContext& ctx, Tally& t) {
    lua_check(ctx, t, "setup: the other armies are neutral to ARMY_1", R"(
        for _, army in ListArmies() do
            if army ~= 'ARMY_1' then SetAlliance('ARMY_1', army, 'Neutral') end
        end
    )");
}

} // namespace

void test_shore(TestContext& ctx) {
    spdlog::info("=== SHORE TEST: units cross between land and water by their layers ===");
    Tally t;
    const int failures_before = osc::test_status::failure_count();

    // A bay on Seton's Clutch's west edge: dry land to z 581, water up to
    // 5.6 deep to z 638, dry land again. Its banks are gentle enough for a
    // hover's and an amphibious unit's footprints (MaxSlope 0.75) all the
    // way across: most of the map's shores are too steep for either, which
    // keeps both on land, as Moho's MobileCheck would. A hover tank, an
    // amphibious one and a land one are sent across it side by side.
    make_peace(ctx, t);
    lua_check(ctx, t, "setup: the bay is where the test expects it", R"(
        local function depth(x, z) return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) end
        for _, x in {40, 44, 48} do
            if depth(x, 572) > 0 or depth(x, 646) > 0 then error('a bank is under water') end
        end
        for _, x in {40, 44} do
            local deepest = 0
            for z = 582, 638 do deepest = math.max(deepest, depth(x, z)) end
            if deepest < 3 then error('the bay is ' .. deepest .. ' deep at x ' .. x) end
        end
        __osc_shore = {}
        for _, row in {{'hover', 'uel0203', 40}, {'amphibious', 'url0203', 44},
                       {'land', 'uel0201', 48}} do
            local unit = CreateUnitHPR(row[2], 'ARMY_1', row[3], GetTerrainHeight(row[3], 572),
                                       572, 0, 0, 0)
            local goal = {row[3], GetTerrainHeight(row[3], 646), 646}
            IssueMove({unit}, goal)
            __osc_shore[row[1]] = {unit = unit, goal = goal, layers = {}}
        end
        function __osc_shore_sample(tick)
            for name, s in __osc_shore do
                if not s.unit:IsDead() then
                    s.layers[s.unit:GetCurrentLayer()] = true
                    local p = s.unit:GetPosition()
                    if not s.arrived and VDist2(p[1], p[3], s.goal[1], s.goal[3]) < 3 then
                        s.arrived = tick
                        LOG('shore-test: the ' .. name .. ' unit arrived at tick ' .. tick)
                    end
                end
            end
        end
    )");
    run_sampled(ctx, t, 1200, "__osc_shore_sample");

    lua_check(ctx, t, "Test 1: the hover tank crosses on the water's surface, and arrives", R"(
        local s = __osc_shore.hover
        if not s.layers.Water then error('it was never on the water') end
        if s.layers.Seabed or s.layers.Sub then error('it went under the water') end
        if not s.arrived then error('it never arrived') end
    )");
    lua_check(ctx, t, "Test 2: the amphibious tank crosses on the seabed, and arrives", R"(
        local s = __osc_shore.amphibious
        if not s.layers.Seabed then error('it was never on the seabed') end
        if s.layers.Water then error('it floated') end
        if not s.arrived then error('it never arrived') end
    )");
    lua_check(ctx, t, "Test 3: the land tank keeps out of the water, and drives round", R"(
        local s = __osc_shore.land
        for layer in s.layers do
            if layer ~= 'Land' then error('it was on the ' .. layer .. ' layer') end
        end
        if not s.arrived then error('it never arrived') end
    )");
    lua_check(ctx, t, "Test 4: each has stopped where it arrived", R"(
        for name, s in __osc_shore do
            if s.unit:IsMoving() then error('the ' .. name .. ' unit is still moving') end
        end
    )");
    t.check(osc::test_status::failure_count() - t.fail == failures_before,
            "Test 5: no script errors");
    spdlog::info("=== SHORE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

void test_choke(TestContext& ctx) {
    spdlog::info("=== CHOKE TEST: a crowd through a wall's gap; a Monkeylord over the wall ===");
    Tally t;
    const int failures_before = osc::test_status::failure_count();

    // On the flat ground east of the map's centre, a wall along z 175 from x
    // 650 to 790 with a gap 4 wide at x 718-722. Twenty-four Strikers south
    // of it are sent to one point north of it. A Monkeylord is sent straight
    // at the wall away from the gap: its class, Amphibious3x3, has
    // IgnoreStructures (retail's footprints.lua), so with Moho's pathing it
    // walks over the wall, where the grid's pathing takes it round.
    make_peace(ctx, t);
    lua_check(ctx, t, "setup: the wall, the crowd and the Monkeylord", R"(
        __osc_gap = {718, 722}
        __osc_wall = {650, 790, 175}
        for x = __osc_wall[1], __osc_wall[2] - 1 do
            if x < __osc_gap[1] or x >= __osc_gap[2] then
                CreateUnitHPR('ueb5101', 'ARMY_1', x + 0.5, GetTerrainHeight(x + 0.5, 175.5),
                              175.5, 0, 0, 0)
            end
        end
        local function spawn(bp, x, z)
            return CreateUnitHPR(bp, 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
        end
        __osc_crowd = {}
        for i = 0, 7 do
            for j = 0, 2 do
                table.insert(__osc_crowd, spawn('uel0201', 706 + 4 * i, 150 + 4 * j))
            end
        end
        __osc_goal = {720, GetTerrainHeight(720, 200), 200}
        IssueMove(__osc_crowd, __osc_goal)
        __osc_big = spawn('url0402', 680, 138)
        __osc_big_goal = {680, GetTerrainHeight(680, 205), 205}
        IssueMove({__osc_big}, __osc_big_goal)

        -- Where each crossed the wall's line, and when each arrived.
        __osc_crossed = {}
        __osc_last_z = {}
        __osc_arrived = {}
        function __osc_choke_sample(tick)
            local function track(key, u, goal, near)
                if u:IsDead() then return end
                local p = u:GetPosition()
                local before = __osc_last_z[key]
                if before and before < __osc_wall[3] and p[3] >= __osc_wall[3] and not __osc_crossed[key] then
                    __osc_crossed[key] = p[1]
                end
                __osc_last_z[key] = p[3]
                if not __osc_arrived[key] and VDist2(p[1], p[3], goal[1], goal[3]) < near then
                    __osc_arrived[key] = tick
                end
            end
            for i, u in __osc_crowd do track(i, u, __osc_goal, 10) end
            track('big', __osc_big, __osc_big_goal, 6)
        end
    )");
    run_sampled(ctx, t, 1500, "__osc_choke_sample");

    lua_check(ctx, t,
              "Test 1: the crowd crosses by the gap: with Moho's pathing none through the "
              "wall, with the grid's every one across",
              R"(
        local through, round, stranded, clipped, last, problems = 0, 0, 0, 0, 0, ''
        for i, u in __osc_crowd do
            local x = __osc_crossed[i]
            local p = u:GetPosition()
            if not x then
                stranded = stranded + 1
                if p[3] > __osc_wall[3] then
                    problems = problems .. string.format(' %d is past the wall and never crossed it;', i)
                end
            elseif x >= __osc_gap[1] - 0.25 and x <= __osc_gap[2] + 0.25 then
                through = through + 1
            elseif x < __osc_wall[1] or x > __osc_wall[2] then
                round = round + 1
            else
                clipped = clipped + 1
            end
            if x and not __osc_arrived[i] then
                problems = problems .. string.format(' %d crossed and never arrived;', i)
            end
            last = math.max(last, __osc_arrived[i] or 0)
        end
        LOG(string.format('choke-test: %d through the gap, %d round the wall, %d through wall cells, '
                          .. '%d stopped short of it; the last in by tick %d',
                          through, round, clipped, stranded, last))
        if __osc_moho_pathing then
            -- Moho's drive moves a unit only where its footprint fits, and
            -- bounces one back from ground it won't (CalcMoveCommon). Its
            -- navigator looks down the way every 7 ticks and asks again
            -- before the wall, so the crowd makes for the gap, not the
            -- wall's face. In retail FA all 24 get across.
            if clipped > 0 then error(clipped .. ' went through wall cells') end
            if stranded > 0 then error(stranded .. ' stopped short of the gap') end
        else
            -- The grid's pathing gets each one across; its smoothed paths
            -- cut the wall's corners (a known gap of the default pathing).
            if stranded > 0 then error(stranded .. ' never crossed') end
        end
        if problems ~= '' then error(problems) end
    )");
    lua_check(ctx, t, "Test 2: the crowd has settled round the point", R"(
        for i, u in __osc_crowd do
            if u:IsMoving() then error('Striker ' .. i .. ' is still moving') end
        end
    )");
    // Pushed apart tick after tick, each is tilted to the ground again: its
    // orientation must stay a rotation (it once grew past 10^4 here).
    lua_check(ctx, t, "Test 2b: each one's orientation is still unit length", R"(
        for i, u in __osc_crowd do
            local q = u:GetOrientation()
            local n = math.sqrt(q[1] * q[1] + q[2] * q[2] + q[3] * q[3] + q[4] * q[4])
            if math.abs(n - 1) > 0.001 then error('Striker ' .. i .. ': |q| ' .. n) end
        end
    )");
    lua_check(ctx, t,
              "Test 3: the Monkeylord crosses over the wall with Moho's pathing, by the gap "
              "or round without it",
              R"(
        local x = __osc_crossed.big
        if not x then error('it never crossed') end
        LOG('choke-test: the Monkeylord crossed at x ' .. x .. ', in by tick ' .. tostring(__osc_arrived.big))
        local in_gap = x >= __osc_gap[1] - 1 and x <= __osc_gap[2] + 1
        local round = x < __osc_wall[1] or x > __osc_wall[2]
        if __osc_moho_pathing then
            if math.abs(x - 680) > 3 then error('it left its line, crossing at x ' .. x) end
        elseif not in_gap and not round then
            error('it crossed the wall at x ' .. x)
        end
        if not __osc_arrived.big then error('it never arrived') end
    )");
    t.check(osc::test_status::failure_count() - t.fail == failures_before,
            "Test 4: no script errors");
    spdlog::info("=== CHOKE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
