// --pod-rebuild-test: a UEF Engineering Station rebuilds its lost pod.
//
// Retail's MaintainPodsState reads the pod's cost through
// brain:GetUnitBlueprint('XEA3204'), an upper-case id; the blueprints are
// keyed lower case. On dry ground away from the starts, ARMY_1's station
// makes its pod; the pod is killed, and the station rebuilds it with no
// thread error (a thread error fails a test run).

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

void test_pod_rebuild(TestContext& ctx) {
    spdlog::info("=== POD REBUILD TEST: a station rebuilds its lost pod ===");
    Tally t;
    const auto check = [&](const std::string& what, const std::string& code) {
        const auto ran = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(ran), ran ? what : what + ": " + ran.error().message);
    };
    const auto pod_id = [&]() -> u32 {
        run_lua(ctx, R"(
            local data = __osc_pr_station.PodData and __osc_pr_station.PodData.PodOne
            local pod = data and data.Active and data.PodHandle
            __osc_pr_pod = (pod and not pod:IsDead()) and tonumber(pod:GetEntityId()) or -1
        )");
        lua_pushstring(ctx.L, "__osc_pr_pod");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 id = osc::sim::to_entity_id(ctx.L, -1);
        lua_pop(ctx.L, 1);
        return id;
    };

    check("an upper-case id finds the pod's blueprint", R"(
        local bp = ArmyBrains[1]:GetUnitBlueprint('XEA3204')
        if not bp or type(bp.Economy.BuildCostEnergy) ~= 'number' then
            error('no blueprint for XEA3204')
        end
    )");

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    spawn_unit(ctx, "__osc_pr_station", "xeb0104", "ARMY_1", *spot);
    run_lua(ctx, "ArmyBrains[1]:GiveResource('ENERGY', 100000)\n"
                 "ArmyBrains[1]:GiveResource('MASS', 10000)\n");
    u32 first = 0;
    for (int i = 0; i < 50 && !first; ++i) {
        ctx.sim.tick();
        first = pod_id();
    }
    t.check(first != 0, "the station makes its pod");
    if (!first) return;

    run_lua(ctx, "__osc_pr_station.PodData.PodOne.PodHandle:Kill()\n");
    u32 rebuilt = 0;
    for (int i = 0; i < 1200 && !rebuilt; ++i) {
        ctx.sim.tick();
        const u32 id = pod_id();
        if (id != 0 && id != first) rebuilt = id;
    }
    t.check(rebuilt != 0, "the station rebuilds the pod");

    spdlog::info("=== POD REBUILD TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
