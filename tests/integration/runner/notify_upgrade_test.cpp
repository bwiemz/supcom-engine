// --notify-upgrade-test: an upgrade's new unit takes over from the old one,
// as Moho's NotifyUpgrade hands it over (faf-re Unit.cpp,
// cfunc_NotifyUpgradeL). Retail's UpgradingState calls it from OnStopBuild
// as the upgrade finishes, then destroys the old unit.
//
// A UEF T1 land factory, half its health gone, in a platoon's Attack squad,
// repeating its queue, with a rally point, an engineer guarding it, and two
// tanks queued behind its upgrade, becomes a T2 factory:
// 1. which has the two tank orders, and not the upgrade;
// 2. whose rally point is the old one's;
// 3. which stands in the old one's place in the platoon, in its squad;
// 4. which repeats its queue, its script told OnStartRepeatQueue;
// 5. at half its health;
// 6. which the engineer now guards.
//
// NotifyUpgrade had been a no-op: every upgrade lost its queue, rally,
// platoon slot, repeat flag, damage and guards.

#include "integration_tests.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <string>

namespace osc::test {

void test_notify_upgrade(TestContext& ctx) {
    spdlog::info("=== NOTIFY UPGRADE TEST: the new unit takes over from the old ===");
    int pass = 0, fail = 0;
    const auto record = [&](bool ok, const std::string& what) {
        if (ok) {
            pass++;
            spdlog::info("[PASS] {}", what);
        } else {
            fail++;
            test_status::fail("[FAIL] {}", what);
        }
    };
    // `code` (Lua) passes when it runs without an error.
    const auto check = [&](const std::string& what, const char* code) {
        auto r = ctx.lua_state.do_string(code);
        record(static_cast<bool>(r), r ? what : what + ": " + r.error().message);
        return static_cast<bool>(r);
    };
    const auto global_number = [&](const char* name) {
        lua_State* L = ctx.lua_state.raw();
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
        return v;
    };

    if (!check("setup", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 50000)
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('MASS', 50000)
        brain:GiveResource('ENERGY', 500000)
        local x, z = brain:GetArmyStartPos()
        local fx, fz = x + 30, z + 30
        local fy = GetTerrainHeight(fx, fz)
        __up_t1 = CreateUnitHPR('ueb0101', 'ARMY_1', fx, fy, fz, 0, 0, 0)
        __up_t1:SetBuildRate(400)
        __up_eng = CreateUnitHPR('uel0105', 'ARMY_1', fx + 6, GetTerrainHeight(fx + 6, fz), fz,
                                 0, 0, 0)
        IssueGuard({__up_eng}, __up_t1)
        __up_platoon = brain:MakePlatoon('upgrade test', '')
        brain:AssignUnitsToPlatoon(__up_platoon, {__up_t1}, 'Attack', 'None')
        __up_t1:SetHealth(nil, __up_t1:GetMaxHealth() / 2)
        __up_rally = {fx + 20, GetTerrainHeight(fx + 20, fz + 10), fz + 10}
        IssueClearFactoryCommands({__up_t1})
        IssueFactoryRallyPoint({__up_t1}, __up_rally)
        __up_t1:SetRepeatQueue(true)
        IssueUpgrade({__up_t1}, 'ueb0201')
        IssueBuildFactory({__up_t1}, 'uel0201', 2)
        -- The T2 factory's class hears OnStartRepeatQueue (rawset: FA's
        -- classes refuse new fields once defined).
        __up_heard = 0
        local cls = import('/units/UEB0201/UEB0201_script.lua').TypeClass
        local old = cls.OnStartRepeatQueue
        rawset(cls, 'OnStartRepeatQueue', function(self)
            __up_heard = __up_heard + 1
            if old then return old(self) end
        end)
        __up_created_by_t1 = false
        -- The upgrade is the T1 factory's focus while it builds; once the
        -- T1 is gone, that is the T2.
        function __up_poll()
            if not __up_t1:BeenDestroyed() then
                __up_t2 = __up_t1:GetFocusUnit() or __up_t2
                if __up_t2 and not __up_t2:BeenDestroyed() and __up_t2:GetCreator() == __up_t1 then
                    __up_created_by_t1 = true
                end
                return false
            end
            __up_t2_id = __up_t2 and tonumber(__up_t2:GetEntityId()) or 0
            return __up_t2 ~= nil and not __up_t2:BeenDestroyed()
        end
    )"))
        return;

    bool upgraded = false;
    for (int i = 0; i < 1500 && !upgraded; ++i) {
        ctx.sim.tick();
        auto r = ctx.lua_state.do_string("__up_done = __up_poll() and 1 or 0");
        upgraded = r && global_number("__up_done") == 1.0;
    }
    record(upgraded, "the T1 factory upgraded to T2");
    if (!upgraded) return;

    {
        const auto id = static_cast<u32>(global_number("__up_t2_id"));
        auto* e = ctx.sim.entity_registry().find(id);
        int tanks = 0, upgrades = 0;
        if (e && e->is_unit())
            for (const auto& cmd : static_cast<sim::Unit*>(e)->command_queue()) {
                if (cmd.type == sim::CommandType::Upgrade) ++upgrades;
                if (cmd.type == sim::CommandType::BuildFactory && cmd.blueprint_id == "uel0201")
                    ++tanks;
            }
        record(tanks == 2 && upgrades == 0, "Test 1: the T2 factory has the two tank orders (" +
                                                std::to_string(tanks) + "), not the upgrade (" +
                                                std::to_string(upgrades) + ")");
    }

    check("Test 2: its rally point is the old one's", R"(
        local p = __up_t2:GetRallyPoint()
        if not p or math.abs(p[1] - __up_rally[1]) > 0.01 or math.abs(p[3] - __up_rally[3]) > 0.01 then
            error('rally ' .. (p and (p[1] .. ',' .. p[3]) or 'nil'))
        end
    )");
    check("Test 3: it stands in the old one's place in the platoon, in its squad", R"(
        local squad = __up_platoon:GetSquadUnits('Attack') or {}
        local found = false
        for _, u in squad do
            if u == __up_t2 then found = true end
            if u == __up_t1 then error('the old factory is still there') end
        end
        if not found then error('not in the Attack squad') end
    )");
    check("Test 4: it repeats its queue, told OnStartRepeatQueue", R"(
        if not __up_t2:IsRepeatQueue() then error('not repeating') end
        if __up_heard ~= 1 then error('heard ' .. __up_heard) end
    )");
    check("Test 5: at half its health", R"(
        local share = __up_t2:GetHealth() / __up_t2:GetMaxHealth()
        if math.abs(share - 0.5) > 0.01 then error('share ' .. share) end
    )");
    check("Test 6: the engineer guards it", R"(
        if __up_eng:GetGuardedUnit() ~= __up_t2 then error('guards another') end
    )");
    check("Test 7: the T1 factory was its creator", R"(
        if not __up_created_by_t1 then error('no creator') end
    )");

    spdlog::info("Notify upgrade test: {}/{} passed", pass, pass + fail);
}

} // namespace osc::test
