// --attack-reach-test: an attack order on a unit closes to the range of the
// weapon it attacks with, as Moho's CUnitAttackTargetTask does: the first of
// the unit's weapons that can attack the target (CAiAttackerImpl::
// GetTargetWeapon), not its longest.
//
// 1. A UEF frigate (UES0103: its gun reaches 28, its anti-air 48), ordered
//    to attack an enemy frigate 60 off, closes to its gun's range and fires
//    it.
// 2. A UEF destroyer (UES0201: guns 60, torpedoes 45), ordered to attack an
//    enemy sub under the water 70 off, closes to its torpedoes' range and
//    fires them.
//
// Before, each stopped at its longest weapon's range (48, 60), where nothing
// it had could hit its target, and never fired.

#include "integration_tests.hpp"
#include "render_probe.hpp" // Tally
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

void test_attack_reach(TestContext& ctx) {
    spdlog::info("=== ATTACK REACH TEST: an attack closes to its target weapon's range ===");
    // Weapons pick only what their army senses (SimState::weapons_know); this
    // tests the order, not intel, so with the fog off it sees everything.
    ctx.sim.set_fog_of_war("none");
    Tally t;
    const auto lua = [&](const char* what, const char* code) {
        auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r),
                r ? std::string(what) : std::string(what) + ": " + r.error().message);
    };

    lua("setup", R"(
        -- Counts a weapon's shots.
        function __osc_ar_count(unit, label)
            local w = unit:GetWeaponByLabel(label)
            w.__osc_shots = 0
            local fire = w.CreateProjectileAtMuzzle
            w.CreateProjectileAtMuzzle = function(self, muzzle)
                self.__osc_shots = self.__osc_shots + 1
                return fire(self, muzzle)
            end
            return w
        end
        function __osc_ar_apart(a, b)
            local p, q = a:GetPosition(), b:GetPosition()
            return VDist2(p[1], p[3], q[1], q[3])
        end
        -- A target that stays where it is and never shoots back.
        function __osc_ar_inert(unit)
            unit:SetCanTakeDamage(false)
            unit:SetFireState('HoldFire')
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
        __osc_ar_sea = {x, z}
    )");

    lua("setup: a frigate ordered to attack an enemy frigate 60 off", R"(
        local x, z = __osc_ar_sea[1], __osc_ar_sea[2]
        __osc_ar_frigate = CreateUnitHPR('ues0103', 'ARMY_1', x, GetSurfaceHeight(x, z - 30), z - 30, 0, 0, 0)
        __osc_ar_ship = CreateUnitHPR('ues0103', 'ARMY_2', x, GetSurfaceHeight(x, z + 30), z + 30, 0, 0, 0)
        __osc_ar_inert(__osc_ar_ship)
        __osc_ar_gun = __osc_ar_count(__osc_ar_frigate, 'MainGun')
        IssueAttack({__osc_ar_frigate}, __osc_ar_ship)
    )");
    const auto frigate_ticks =
        tick_until(ctx, "Test 1: the frigate closes to its gun's range and fires it", 600,
                   "__osc_ar_gun.__osc_shots > 0");
    lua("Test 1 (checked): it fired from within the gun's 28", R"(
        local d = __osc_ar_apart(__osc_ar_frigate, __osc_ar_ship)
        LOG('the frigate fired from ' .. d)
        if d > 28 then error('it stands ' .. d .. ' off') end
    )");
    spdlog::info("the frigate's gun fired after {} ticks", frigate_ticks.value_or(-1));
    tick_for(ctx, 50);
    lua("Test 1b: and stays there, short of its target", R"(
        local d = __osc_ar_apart(__osc_ar_frigate, __osc_ar_ship)
        if d > 28 then error('it is ' .. d .. ' off') end
        if d < 20 then error('it went on to ' .. d) end
    )");

    lua("setup: a destroyer ordered to attack an enemy sub under the water 70 off", R"(
        IssueClearCommands({__osc_ar_frigate})
        __osc_ar_frigate:Destroy()
        __osc_ar_ship:Destroy()
        local x, z = __osc_ar_sea[1], __osc_ar_sea[2]
        __osc_ar_sub = CreateUnitHPR('ues0203', 'ARMY_2', x, GetSurfaceHeight(x, z + 35), z + 35, 0, 0, 0)
        __osc_ar_inert(__osc_ar_sub)
    )");
    tick_until(ctx, "the sub is under the water", 60, "__osc_ar_sub:GetCurrentLayer() == 'Sub'");
    lua("setup: the destroyer", R"(
        local x, z = __osc_ar_sea[1], __osc_ar_sea[2]
        __osc_ar_destroyer = CreateUnitHPR('ues0201', 'ARMY_1', x, GetSurfaceHeight(x, z - 35), z - 35, 0, 0, 0)
        __osc_ar_torpedo = __osc_ar_count(__osc_ar_destroyer, 'Torpedo01')
        IssueAttack({__osc_ar_destroyer}, __osc_ar_sub)
    )");
    const auto destroyer_ticks =
        tick_until(ctx, "Test 2: the destroyer closes to its torpedoes' range and fires them", 600,
                   "__osc_ar_torpedo.__osc_shots > 0");
    lua("Test 2 (checked): it fired from within the torpedoes' 45, the sub still under", R"(
        local d = __osc_ar_apart(__osc_ar_destroyer, __osc_ar_sub)
        LOG('the destroyer fired from ' .. d)
        if d > 45 then error('it stands ' .. d .. ' off') end
        if __osc_ar_sub:GetCurrentLayer() ~= 'Sub' then
            error('the sub is on ' .. __osc_ar_sub:GetCurrentLayer())
        end
    )");

    spdlog::info("the destroyer's torpedoes fired after {} ticks", destroyer_ticks.value_or(-1));
    spdlog::info("Attack reach test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
