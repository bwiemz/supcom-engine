// --build-site-join-test: builders sent to one site build one structure there,
// and none where another structure stands (faf-re CUnitMobileBuildTask,
// TASKSTATE_Processing).

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace osc::test {

namespace {

std::vector<u32> global_ids(lua_State* L, const char* name) {
    std::vector<u32> ids;
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_isnumber(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            ids.push_back(static_cast<u32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return ids;
}

} // namespace

void test_build_site_join(TestContext& ctx) {
    spdlog::info("=== BUILD SITE JOIN TEST: builders sent to one site build one structure ===");
    Tally t;
    lua_State* L = ctx.L;
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
    if (!run("engineers of two armies, their sites clear, resources to spare", R"(
        local near = GetUnitsInRect(240, 770, 330, 802)
        if near and table.getn(near) > 0 then error('units near the sites') end
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 100000)
        brain:GiveResource('MASS', 100000)
        brain:GiveResource('ENERGY', 100000)
        local function engineers(x0)
            local list, ids = {}, {}
            for i = 0, 2 do
                local x = x0 + 4 * i
                local e = CreateUnitHPR('uel0105', 'ARMY_1', x, GetTerrainHeight(x, 790), 790, 0, 0, 0)
                table.insert(list, e)
                table.insert(ids, e:GetEntityId())
            end
            return list, ids
        end
        __osc_join_player, __osc_join_player_ids = engineers(258)
        __osc_join_own = engineers(298)
        __osc_join_apart = CreateUnitHPR('uel0105', 'ARMY_1', 312, GetTerrainHeight(312, 790), 790, 0, 0, 0)
        local foe = GetArmyBrain('ARMY_2')
        foe:GiveStorage('MASS', 100000)
        foe:GiveResource('MASS', 100000)
        foe:GiveResource('ENERGY', 100000)
        local function engineer(army, x)
            return CreateUnitHPR('uel0105', army, x, GetTerrainHeight(x, 776), 776, 0, 0, 0)
        end
        local function listen(e)
            local heard = {}
            for _, name in {'OnStartBuild', 'OnStopBuild', 'OnFailedToBuild'} do
                local n, f = name, e[name]
                e[n] = function(self, a, b)
                    table.insert(heard, n)
                    return f(self, a, b)
                end
            end
            return heard
        end
        __osc_join_mine = engineer('ARMY_1', 258)
        __osc_join_foe = engineer('ARMY_2', 266)
        CreateUnitHPR('ueb1101', 'ARMY_2', 282.5, GetTerrainHeight(282.5, 772.5), 772.5, 0, 0, 0)
        __osc_join_onto = engineer('ARMY_1', 286)
        __osc_join_onto_heard = listen(__osc_join_onto)
        __osc_join_foe_frame = engineer('ARMY_2', 298)
        __osc_join_radar = engineer('ARMY_1', 306)
        __osc_join_radar_heard = listen(__osc_join_radar)
        function __osc_join_on(bp, x, z)
            local found = {}
            for _, u in GetUnitsInRect(x - 3, z - 3, x + 3, z + 3) or {} do
                if u:GetBlueprint().BlueprintId == bp then
                    table.insert(found, u)
                end
            end
            return found
        end
        function __osc_join_refused(e, heard)
            if table.getn(e:GetCommandQueue()) ~= 0 then error('it still has its order') end
            if table.concat(heard, ' ') ~= 'OnFailedToBuild' then
                error('it heard ' .. table.concat(heard, ' '))
            end
        end
    )")) {
        return;
    }
    ticks(5);
    {
        sim::UnitCommand build;
        build.type = sim::CommandType::BuildMobile;
        build.blueprint_id = "ueb1101";
        build.target_pos = {262.0f, 0.0f, 784.0f};
        ctx.sim.set_human_input_active(true);
        ctx.sim.route_player_command(global_ids(L, "__osc_join_player_ids"), build, true);
        ctx.sim.set_human_input_active(false);
    }
    run("each of the others sent by its own order, one more to a site beside theirs", R"(
        for _, e in __osc_join_own do
            IssueBuildMobile({e}, {302, GetTerrainHeight(302, 784), 784}, 'ueb1101', {})
        end
        IssueBuildMobile({__osc_join_apart}, {310, GetTerrainHeight(310, 784), 784}, 'ueb1101', {})
        IssueBuildMobile({__osc_join_mine}, {262, GetTerrainHeight(262, 772), 772}, 'ueb1101', {})
        IssueBuildMobile({__osc_join_onto}, {282, GetTerrainHeight(282, 772), 772}, 'ueb1101', {})
        IssueBuildMobile({__osc_join_foe_frame}, {302, GetTerrainHeight(302, 772), 772}, 'ueb1101', {})
    )");
    ticks(5);
    run("another army's engineers sent to the sites the first ones' frames took", R"(
        IssueBuildMobile({__osc_join_foe}, {262, GetTerrainHeight(262, 772), 772}, 'ueb1101', {})
        IssueBuildMobile({__osc_join_radar}, {302, GetTerrainHeight(302, 772), 772}, 'ueb3101', {})
    )");
    ticks(25);
    run("1: three engineers given one player's order build one power generator, together", R"(
        local pgens = __osc_join_on('ueb1101', 262.5, 784.5)
        if table.getn(pgens) ~= 1 then error(table.getn(pgens) .. ' power generators on the site') end
        for i, e in __osc_join_player do
            if e:GetFocusUnit() ~= pgens[1] then error('engineer ' .. i .. ' builds something else') end
        end
    )");
    run("2: three engineers sent there by orders of their own build one, together", R"(
        local pgens = __osc_join_on('ueb1101', 302.5, 784.5)
        if table.getn(pgens) ~= 1 then error(table.getn(pgens) .. ' power generators on the site') end
        for i, e in __osc_join_own do
            if e:GetFocusUnit() ~= pgens[1] then error('engineer ' .. i .. ' builds something else') end
        end
    )");
    run("3: one sent to the site beside theirs builds its own there", R"(
        local pgens = __osc_join_on('ueb1101', 310.5, 784.5)
        if table.getn(pgens) ~= 1 then error(table.getn(pgens) .. ' power generators on its site') end
        if __osc_join_apart:GetFocusUnit() ~= pgens[1] then error('it builds something else') end
    )");
    run("4: one sent to build on another army's frame of the same structure builds that one", R"(
        local pgens = __osc_join_on('ueb1101', 262.5, 772.5)
        if table.getn(pgens) ~= 1 then error(table.getn(pgens) .. ' power generators on the site') end
        if pgens[1]:GetArmy() ~= 1 then error('the frame went to army ' .. pgens[1]:GetArmy()) end
        if __osc_join_foe:GetFocusUnit() ~= pgens[1] then error('it builds something else') end
    )");
    run("5: one sent to a site another army's structure stands on builds nothing, and fails", R"(
        local pgens = __osc_join_on('ueb1101', 282.5, 772.5)
        if table.getn(pgens) ~= 1 then error(table.getn(pgens) .. ' power generators on the site') end
        __osc_join_refused(__osc_join_onto, __osc_join_onto_heard)
    )");
    run("6: one sent to build a radar on another army's frame builds nothing, and fails", R"(
        if table.getn(__osc_join_on('ueb3101', 302.5, 772.5)) ~= 0 then error('a radar on the site') end
        if table.getn(__osc_join_on('ueb1101', 302.5, 772.5)) ~= 1 then error('the frame is gone') end
        __osc_join_refused(__osc_join_radar, __osc_join_radar_heard)
    )");
    ticks(120);
    run("7: the joined power generators are finished, one to a site", R"(
        for _, x in {262.5, 302.5} do
            local pgens = __osc_join_on('ueb1101', x, 784.5)
            if table.getn(pgens) ~= 1 then error(table.getn(pgens) .. ' power generators at ' .. x) end
            if pgens[1]:GetFractionComplete() < 1 then error('the one at ' .. x .. ' is unfinished') end
        end
    )");
    spdlog::info("Build site join test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
