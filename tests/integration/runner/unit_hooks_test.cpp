// --unit-hooks-test: the unit hooks Moho calls as a unit is made and as its
// script switches its economy, and the sacrifice's donation (faf-re
// Unit.cpp, CUnitSacrificeTask.cpp).
//
// 1. A unit starts in its blueprint's AI.InitialAutoMode, and its script
//    hears OnAutoModeOn (a tactical missile launcher) or OnAutoModeOff (a
//    tank) before OnPreCreate.
// 2. unit:SetAutoMode tells the script every time, changed or not.
// 3. unit:SetConsumptionActive tells the script OnConsumptionActive or
//    OnConsumptionInActive when its flag changes, and only then.
// 4. unit:SetProductionActive tells it OnProductionActive or
//    OnProductionInActive every time.
// 5. A retail mass fabricator turned off stops producing: its
//    OnConsumptionInActive turns its production off.
// 6. An Aeon engineer sacrificed into a damaged generator mends it by
//    min(its mass, its energy share): its build cost times its
//    SacrificeMassMult/SacrificeEnergyMult over the generator's.
// 7. Sacrificed into an enhancing ACU, it adds that share of the
//    enhancement's costs to the enhancement's work.
//
// The engine had never called the auto mode, consumption or production
// hooks, and a sacrifice donated its builder's last target's cost, nothing.

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

void test_unit_hooks(TestContext& ctx) {
    spdlog::info("=== UNIT HOOKS TEST: auto mode, economy switches, sacrifice ===");
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
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };
    // The C++ unit a Lua global names.
    const auto unit = [&](const char* global) -> sim::Unit* {
        lua_State* L = ctx.lua_state.raw();
        lua_pushstring(L, global);
        lua_rawget(L, LUA_GLOBALSINDEX);
        sim::Unit* u = nullptr;
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "GetEntityId");
            lua_gettable(L, -2);
            lua_pushvalue(L, -2);
            if (lua_pcall(L, 1, 1, 0) == 0) {
                const auto id = static_cast<u32>(lua_tonumber(L, -1));
                auto* e = ctx.sim.entity_registry().find(id);
                if (e && e->is_unit() && !e->destroyed()) u = static_cast<sim::Unit*>(e);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return u;
    };

    if (!check("setup", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 50000)
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('MASS', 50000)
        brain:GiveResource('ENERGY', 500000)
        local x, z = brain:GetArmyStartPos()
        function __hooks_make(bp, dx, dz)
            local px, pz = x + 30 + dx, z + 30 + dz
            return CreateUnitHPR(bp, 'ARMY_1', px, GetTerrainHeight(px, pz), pz, 0, 0, 0)
        end
        -- Each class's hooks, recorded in order as the next unit is made
        -- (rawset: FA's classes refuse new fields once defined). The class
        -- is the blueprint script's TypeClass, as the engine loads it.
        __hooks_log = {}
        function __hooks_watch(id)
            local up = string.upper(id)
            local cls = import('/units/' .. up .. '/' .. up .. '_script.lua').TypeClass
            for _, each in {'OnAutoModeOn', 'OnAutoModeOff', 'OnPreCreate'} do
                local name = each -- (Lua 5.0's loop variable isn't one per pass)
                local old = cls[name]
                rawset(cls, name, function(self, a, b, c)
                    table.insert(__hooks_log, name .. ':' .. tostring(self:GetAutoMode()))
                    if old then return old(self, a, b, c) end
                end)
            end
        end
        __hooks_watch('ueb2108')
        __hooks_watch('uel0201')
    )"))
        return;

    check("Test 1: a unit starts in its InitialAutoMode, and hears it before OnPreCreate", R"(
        __hooks_log = {}
        __hooks_tml = __hooks_make('ueb2108', 0, 0)
        local tml = table.concat(__hooks_log, ' ')
        __hooks_log = {}
        __hooks_tank = __hooks_make('uel0201', 10, 0)
        local tank = table.concat(__hooks_log, ' ')
        if tml ~= 'OnAutoModeOn:true OnPreCreate:true' then error('launcher: ' .. tml) end
        if tank ~= 'OnAutoModeOff:false OnPreCreate:false' then error('tank: ' .. tank) end
    )");

    check("Test 2: SetAutoMode tells the script every time", R"(
        __hooks_log = {}
        __hooks_tml:SetAutoMode(true)
        __hooks_tml:SetAutoMode(false)
        __hooks_tml:SetAutoMode(false)
        local got = table.concat(__hooks_log, ' ')
        if got ~= 'OnAutoModeOn:true OnAutoModeOff:false OnAutoModeOff:false' then
            error(got)
        end
    )");

    check("Test 3: SetConsumptionActive tells the script when its flag changes", R"(
        local heard = {}
        __hooks_tank.OnConsumptionActive = function(self) table.insert(heard, 'on') end
        __hooks_tank.OnConsumptionInActive = function(self) table.insert(heard, 'off') end
        __hooks_tank:SetConsumptionActive(true)
        __hooks_tank:SetConsumptionActive(true)
        __hooks_tank:SetConsumptionActive(false)
        __hooks_tank:SetConsumptionActive(false)
        __hooks_tank:SetConsumptionActive(true)
        local got = table.concat(heard, ' ')
        if got ~= 'on off on' then error(got) end
    )");

    check("Test 4: SetProductionActive tells the script every time", R"(
        local heard = {}
        __hooks_tank.OnProductionActive = function(self) table.insert(heard, 'on') end
        __hooks_tank.OnProductionInActive = function(self) table.insert(heard, 'off') end
        __hooks_tank:SetProductionActive(true)
        __hooks_tank:SetProductionActive(true)
        __hooks_tank:SetProductionActive(false)
        local got = table.concat(heard, ' ')
        if got ~= 'on on off' then error(got) end
    )");

    // 5. A T2 mass fabricator, finished: producing; turned off, not.
    if (!check("Test 5 setup", R"(
        __hooks_fab = __hooks_make('ueb1104', 0, 20)
    )"))
        return;
    run(3);
    const sim::Unit* fab = unit("__hooks_fab");
    const bool producing = fab && fab->economy().production_active;
    check("Test 5 switch", R"(
        __hooks_fab:SetConsumptionActive(false)
    )");
    run(1);
    fab = unit("__hooks_fab");
    record(producing && fab && !fab->economy().production_active,
           "Test 5: a mass fabricator produces, and its consumption turned off stops it");

    check("Test 6: a sacrifice mends its target by its build cost times its multipliers", R"(
        __hooks_pgen = CreateUnitHPR('uab1101', 'ARMY_1', __hooks_tank:GetPosition()[1] + 10,
            0, __hooks_tank:GetPosition()[3], 0, 0, 0)
        local p = __hooks_pgen:GetPosition()
        __hooks_pgen:SetHealth(nil, 1)
        __hooks_eng = CreateUnitHPR('ual0105', 'ARMY_1', p[1] + 3, p[2], p[3], 0, 0, 0)
        local eng = __hooks_eng:GetBlueprint().Economy
        local gen = __hooks_pgen:GetBlueprint().Economy
        __hooks_step = math.min(eng.BuildCostMass * eng.SacrificeMassMult / gen.BuildCostMass,
                                eng.BuildCostEnergy * eng.SacrificeEnergyMult / gen.BuildCostEnergy)
        __hooks_want = math.min(__hooks_pgen:GetMaxHealth(),
                                1 + __hooks_pgen:GetMaxHealth() * __hooks_step)
        IssueSacrifice({__hooks_eng}, __hooks_pgen)
    )");
    run(60);
    check("Test 6: the generator is mended, the engineer gone", R"(
        local got = __hooks_pgen:GetHealth()
        if __hooks_step <= 0 or math.abs(got - __hooks_want) > 0.01 then
            error(string.format('health %g, want %g (step %g)', got, __hooks_want, __hooks_step))
        end
        if not __hooks_eng:BeenDestroyed() then error('the engineer is still here') end
    )");

    // 7. Into an enhancing ACU, the donation goes to the enhancement's work:
    // its share of the costs retail's OnWorkBegin put on the unit (which
    // gives the energy cost for both). Over one tick the work rises by its
    // own rate plus the step.
    if (!check("Test 7 setup", R"(
        __hooks_acu = GetEntityById(__osc_test_acu_id(1))
        IssueEnhancement({__hooks_acu}, 'AdvancedEngineering')
    )"))
        return;
    run(5);
    check("Test 7 sacrifice", R"(
        if not __hooks_acu:IsUnitState('Enhancing') then error('not enhancing') end
        local p = __hooks_acu:GetPosition()
        __hooks_eng2 = CreateUnitHPR('ual0105', 'ARMY_1', p[1] + 3, p[2], p[3], 0, 0, 0)
        local eng = __hooks_eng2:GetBlueprint().Economy
        __hooks_step = math.min(
            eng.BuildCostMass * eng.SacrificeMassMult / __hooks_acu.WorkItemBuildCostMass,
            eng.BuildCostEnergy * eng.SacrificeEnergyMult / __hooks_acu.WorkItemBuildCostEnergy)
        __hooks_w0 = __hooks_acu.WorkProgress
    )");
    run(1);
    check("Test 7 rate", R"(
        __hooks_w1 = __hooks_acu.WorkProgress
        IssueSacrifice({__hooks_eng2}, __hooks_acu)
    )");
    run(1);
    check("Test 7: a sacrifice into an enhancing unit adds its share to the enhancement's work", R"(
        local w2 = __hooks_acu.WorkProgress
        local own = __hooks_w1 - __hooks_w0
        local got = w2 - __hooks_w1 - own
        if __hooks_step <= 0 or math.abs(got - __hooks_step) > 1e-3 then
            error(string.format('step %g, want %g (own rate %g)', got, __hooks_step, own))
        end
        if not __hooks_eng2:BeenDestroyed() then error('the engineer is still here') end
    )");

    spdlog::info("Unit hooks test: {}/{} passed", pass, pass + fail);
}

} // namespace osc::test
