// --unit-cap-test: the army's unit cap, as Moho's Sim::CreateUnit holds it
// (faf-re Sim.cpp, CArmyImpl::GetArmyUnitCostTotal).
//
// 1. Each unit counts its blueprint's General.CapCost: a wall 0.1, a tank 1.
// 2. Over the cap CreateUnitHPR fails with an error, and the brain hears
//    OnUnitCapLimitReached; a wall still fits where a tank doesn't.
// 3. SetIgnoreArmyUnitCap lets the army past its cap, and
//    CreateInitialArmyUnit isn't held to it.
// 4. A dying unit counts no more.
// 5. A factory at the cap keeps its order and tries again every 10 ticks,
//    the brain hearing each try; it builds once there is room.
// 6. An upgrade isn't held to the cap.
// 7. ChangeUnitArmy into an army at its cap makes nothing, the unit stays
//    its old army's, and the new army's brain hears OnFailedUnitTransfer.
//
// The engine had checked the cap only when a build order started, counted
// every unit as 1, dropped the order at the cap, and never capped a
// script's units or a transfer.

#include "integration_tests.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

void test_unit_cap(TestContext& ctx) {
    spdlog::info("=== UNIT CAP TEST: Moho's Sim::CreateUnit gate ===");
    int pass = 0, fail = 0;
    // `code` (Lua) passes when it runs without an error.
    const auto check = [&](const std::string& what, const char* code) {
        auto r = ctx.lua_state.do_string(code);
        if (r) {
            pass++;
            spdlog::info("[PASS] {}", what);
        } else {
            fail++;
            test_status::fail("[FAIL] {}: {}", what, r.error().message);
        }
        return static_cast<bool>(r);
    };
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };

    if (!check("setup", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 50000)
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('MASS', 50000)
        brain:GiveResource('ENERGY', 500000)
        local x, z = brain:GetArmyStartPos()
        -- A spot (dx, dz) off the start: x, y, z.
        function __cap_at(dx, dz)
            local px, pz = x + 30 + dx, z + 30 + dz
            return px, GetTerrainHeight(px, pz), pz
        end
        function __cap_make(bp, army, dx, dz)
            local px, py, pz = __cap_at(dx, dz)
            return CreateUnitHPR(bp, army, px, py, pz, 0, 0, 0)
        end
        -- The cap just above the army's units: room for less than a tank.
        function __cap_fill(army)
            SetArmyUnitCap(army, math.ceil(GetArmyUnitCostTotal(army)))
            local room = GetArmyUnitCap(army) - GetArmyUnitCostTotal(army)
            if room < 0 or room >= 1 then error('room ' .. room) end
            return room
        end
        __cap_heard = 0
        __cap_failed = 0
        brain.OnUnitCapLimitReached = function(self) __cap_heard = __cap_heard + 1 end
        GetArmyBrain('ARMY_2').OnFailedUnitTransfer = function(self)
            __cap_failed = __cap_failed + 1
        end
    )"))
        return;

    check("Test 1: a wall counts 0.1 against the cap and a tank 1", R"(
        local t0 = GetArmyUnitCostTotal(1)
        __cap_wall = __cap_make('ueb5101', 'ARMY_1', 0, 0)
        local t1 = GetArmyUnitCostTotal(1)
        __cap_tank = __cap_make('uel0201', 'ARMY_1', 10, 0)
        local t2 = GetArmyUnitCostTotal(1)
        if math.abs(t1 - t0 - 0.1) > 1e-4 or math.abs(t2 - t1 - 1) > 1e-4 then
            error(string.format('totals %g, %g, %g', t0, t1, t2))
        end
    )");

    check("Test 2: over the cap CreateUnitHPR fails and the brain hears it; a wall still fits",
          R"(
        local room = __cap_fill(1)
        local before = GetArmyUnitCostTotal(1)
        local px, py, pz = __cap_at(20, 0)
        local ok, err = pcall(CreateUnitHPR, 'uel0201', 'ARMY_1', px, py, pz, 0, 0, 0)
        if ok then error('a tank was made with room ' .. room) end
        if not string.find(tostring(err), 'CreateUnitHPR%(uel0201%) failed') then
            error('error: ' .. tostring(err))
        end
        if __cap_heard ~= 1 then error('the brain heard ' .. __cap_heard) end
        if GetArmyUnitCostTotal(1) ~= before then error('the total moved') end
        if not __cap_make('ueb5101', 'ARMY_1', 0, 5) then error('no wall') end
    )");

    check("Test 3: SetIgnoreArmyUnitCap lets the army past its cap, and CreateInitialArmyUnit "
          "isn't held to it",
          R"(
        __cap_fill(1)
        SetIgnoreArmyUnitCap(1, true)
        __cap_tank2 = __cap_make('uel0201', 'ARMY_1', 20, 0)
        SetIgnoreArmyUnitCap(1, false)
        if not __cap_tank2 then error('ignoring, no tank') end
        __cap_fill(1)
        if not CreateInitialArmyUnit('ARMY_1', 'uel0201') then error('no initial unit') end
        if __cap_heard ~= 1 then error('the brain heard ' .. __cap_heard) end
    )");

    check("Test 4: a dying unit counts no more", R"(
        local before = GetArmyUnitCostTotal(1)
        __cap_tank2:Kill()
        local after = GetArmyUnitCostTotal(1)
        if math.abs(before - after - 1) > 1e-4 then
            error(string.format('%g before, %g after', before, after))
        end
    )");

    // 5. The factory, made past the cap, then held to it.
    if (!check("Test 5 setup", R"(
        SetIgnoreArmyUnitCap(1, true)
        __cap_factory = __cap_make('ueb0101', 'ARMY_1', -30, 0)
        __cap_upgrader = __cap_make('ueb0101', 'ARMY_1', -30, 30)
        SetIgnoreArmyUnitCap(1, false)
        __cap_fill(1)
        __cap_heard_before = __cap_heard
        IssueBuildFactory({__cap_factory}, 'uel0201', 1)
    )"))
        return;
    run(35);
    check("Test 5: a factory at the cap keeps its order, trying every 10 ticks", R"(
        local tries = __cap_heard - __cap_heard_before
        if tries < 3 or tries > 4 then error(tries .. ' tries in 35 ticks') end
        if table.getn(__cap_factory:GetCommandQueue()) ~= 1 then error('the order went') end
        if __cap_factory:IsUnitState('Building') then error('building at the cap') end
        SetArmyUnitCap(1, GetArmyUnitCap(1) + 1)
    )");
    run(12);
    check("Test 5: with room the factory builds", R"(
        if not __cap_factory:IsUnitState('Building') then error('not building') end
    )");

    if (!check("Test 6 setup", R"(
        __cap_fill(1)
        IssueUpgrade({__cap_upgrader}, 'ueb0201')
    )"))
        return;
    run(5);
    check("Test 6: an upgrade isn't held to the cap", R"(
        if not __cap_upgrader:IsUnitState('Upgrading') then error('not upgrading') end
    )");

    check("Test 7: ChangeUnitArmy into an army at its cap makes nothing, and that brain hears it",
          R"(
        __cap_fill(2)
        local moved = ChangeUnitArmy(__cap_tank, 2)
        if moved then error('a unit was made for army 2') end
        if __cap_failed ~= 1 then error('the brain heard ' .. __cap_failed) end
        if __cap_tank:BeenDestroyed() or __cap_tank:GetArmy() ~= 1 then
            error('the tank left army 1')
        end
    )");

    spdlog::info("Unit cap test: {}/{} passed", pass, pass + fail);
}

} // namespace osc::test
