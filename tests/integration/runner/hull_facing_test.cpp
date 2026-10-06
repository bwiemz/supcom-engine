// --hull-facing-test: retail units turning their hulls to their weapons'
// work, as Moho's CalcMoveCommon does (parity PR 5b): a UEF battleship
// puts its target 60 degrees off its bow (AttackAngle) so its aft turret
// bears, and a mobile missile launcher, whose launcher can't turn, pivots
// to its target and fires.

#include "integration_tests.hpp"
#include "render_probe.hpp" // Tally

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

namespace osc::test {

void test_hull_facing(TestContext& ctx) {
    spdlog::info("=== HULL FACING TEST: slaved weapons and AttackAngle turn the hull ===");
    Tally t;
    const auto lua = [&](const char* what, const char* code) {
        auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r),
                r ? std::string(what) : std::string(what) + ": " + r.error().message);
    };
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };

    lua("setup", R"(
        -- Counts each weapon's shells by label.
        function __osc_count_shots(unit, label)
            local w = unit:GetWeaponByLabel(label)
            w.__osc_shots = 0
            local fire = w.CreateProjectileAtMuzzle
            w.CreateProjectileAtMuzzle = function(self, muzzle)
                self.__osc_shots = self.__osc_shots + 1
                return fire(self, muzzle)
            end
            return w
        end
        function __osc_off_bow(unit, target)
            local p, q = unit:GetPosition(), target:GetPosition()
            local bearing = math.deg(math.atan2(q[1] - p[1], q[3] - p[3]))
            local h = math.deg(unit:GetHeading())
            local d = bearing - h
            while d > 180 do d = d - 360 end
            while d <= -180 do d = d + 360 end
            return d
        end
        -- Deep, open water, 300 across.
        local x, z
        for tz = 150, 850, 25 do
            for tx = 150, 850, 25 do
                if not x then
                    local ok = true
                    for dx = -150, 150, 50 do
                        for dz = -150, 150, 50 do
                            if GetSurfaceHeight(tx + dx, tz + dz) - GetTerrainHeight(tx + dx, tz + dz) < 6 then ok = false end
                        end
                    end
                    if ok then x, z = tx, tz end
                end
            end
        end
        if not x then error('no open water') end
        __osc_sea = {x, z}
    )");
    lua("setup: a battleship ordered to attack a frigate ahead", R"(
        local x, z = __osc_sea[1], __osc_sea[2]
        __osc_summit = CreateUnitHPR('ues0302', 'ARMY_1', x, GetSurfaceHeight(x, z - 60), z - 60, 0, 0, 0)
        __osc_frigate = CreateUnitHPR('ues0103', 'ARMY_2', x, GetSurfaceHeight(x, z + 50), z + 50, 0, 0, 0)
        __osc_frigate:SetCanTakeDamage(false)
        __osc_frigate:SetFireState('HoldFire')
        __osc_back = __osc_count_shots(__osc_summit, 'BackTurret')
        __osc_front = __osc_count_shots(__osc_summit, 'FrontTurret01')
        IssueAttack({__osc_summit}, __osc_frigate)
    )");
    run(300);
    lua("Test 1: the battleship puts its target 60 off its bow", R"(
        local off = math.abs(__osc_off_bow(__osc_summit, __osc_frigate))
        if math.abs(off - 60) > 3 then error('the frigate is ' .. off .. ' off the bow') end
    )");
    lua("Test 2: its aft turret bears and fires, as do the forward ones", R"(
        if __osc_front.__osc_shots == 0 then error('the forward turret never fired') end
        if __osc_back.__osc_shots == 0 then error('the aft turret never fired') end
    )");

    lua("setup: a mobile missile launcher with an enemy on its beam", R"(
        IssueClearCommands({__osc_summit})
        __osc_summit:Destroy()
        __osc_frigate:Destroy()
        -- Dry, level ground: 200 east of the sea's edge, searched.
        local x, z
        for tz = 150, 850, 25 do
            for tx = 150, 850, 25 do
                if not x and GetTerrainHeight(tx, tz) > GetSurfaceHeight(tx, tz) - 0.01
                    and GetTerrainHeight(tx + 40, tz) > GetSurfaceHeight(tx + 40, tz) - 0.01
                    and math.abs(GetTerrainHeight(tx + 40, tz) - GetTerrainHeight(tx, tz)) < 3 then
                    x, z = tx, tz
                end
            end
        end
        if not x then error('no dry ground') end
        __osc_mml = CreateUnitHPR('uel0111', 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
        __osc_pgen = CreateUnitHPR('ueb1101', 'ARMY_2', x + 40, GetTerrainHeight(x + 40, z), z, 0, 0, 0)
        __osc_pgen:SetCanTakeDamage(false)
        __osc_missiles = __osc_count_shots(__osc_mml, 'MissileWeapon')
    )");
    run(150);
    lua("Test 3: the launcher, which can't turn, pivots to the target and fires", R"(
        local off = math.abs(__osc_off_bow(__osc_mml, __osc_pgen))
        if off > 1 then error('the target is ' .. off .. ' off its bow') end
        if __osc_missiles.__osc_shots == 0 then error('it never fired') end
    )");

    spdlog::info("Hull facing test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
