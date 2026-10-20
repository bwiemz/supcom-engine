// --assist-beam-test: a commander helping a factory's upgrade lets its build effects go when
// the help ends -- by a new order, or by the upgrade finishing -- not at its next build
// (retail's OnStopBuild destroys BuildEffectsBag, and the effects leave the frame's snapshot).

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/ieffect.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

namespace {

constexpr int kWaitTicks = 3000;

} // namespace

void test_assist_beam(TestContext& ctx) {
    spdlog::info("=== ASSIST BEAM TEST: a commander's build effects end with its help ===");
    Tally t;
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
    const auto global_number = [&](const char* name) {
        lua_pushstring(ctx.L, name);
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const u32 n = static_cast<u32>(lua_tonumber(ctx.L, -1));
        lua_pop(ctx.L, 1);
        return n;
    };
    // The beams the next frame draws from or to the commander.
    const auto drawn_beams = [&](u32 commander) {
        sim::WorldSnapshot snap;
        sim::capture_world(ctx.sim, snap);
        int n = 0;
        for (const auto& fx : snap.effects) {
            const bool beam = fx.type == sim::EffectType::BEAM_EMITTER ||
                              fx.type == sim::EffectType::ATTACHED_BEAM ||
                              fx.type == sim::EffectType::BEAM_ENTITY_TO_ENTITY;
            n += beam && (fx.entity_id == commander || fx.target_entity_id == commander) ? 1 : 0;
        }
        return n;
    };
    // Ticks until the Lua expression holds, at most kWaitTicks.
    const auto until = [&](const std::string& what, const std::string& expr) {
        for (int i = 0; i < kWaitTicks; ++i) {
            const auto r = ctx.lua_state.do_string("__osc_ab_hold = (" + expr + ")");
            lua_pushstring(ctx.L, "__osc_ab_hold");
            lua_rawget(ctx.L, LUA_GLOBALSINDEX);
            const bool held = r && lua_toboolean(ctx.L, -1) != 0;
            lua_pop(ctx.L, 1);
            if (held) {
                t.check(true, what);
                return true;
            }
            ctx.sim.tick();
        }
        t.check(false, fmt::format("{}: not within {} ticks", what, kWaitTicks));
        return false;
    };

    if (!run("two commanders, each beside a land factory, resources to spare", R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('MASS', 100000)
        brain:GiveResource('MASS', 100000)
        brain:GiveResource('ENERGY', 100000)
        local h = GetTerrainHeight
        local function pair(x)
            local p = {
                acu = CreateUnitHPR('uel0001', 'ARMY_1', x, h(x, 785), 785, 0, 0, 0),
                fac = CreateUnitHPR('ueb0101', 'ARMY_1', x + 4, h(x + 4, 780), 780, 0, 0, 0),
                heard = {},
            }
            for _, name in {'OnStartBuild', 'OnStopBuild'} do
                local n, f = name, p.acu[name]
                p.acu[n] = function(self, a, b)
                    table.insert(p.heard, n)
                    return f(self, a, b)
                end
            end
            IssueUpgrade({p.fac}, 'ueb0201')
            return p
        end
        __osc_ab_moved = pair(258)
        __osc_ab_done = pair(300)
        __osc_ab_moved_id = __osc_ab_moved.acu:GetEntityId()
        __osc_ab_done_id = __osc_ab_done.acu:GetEntityId()
        function __osc_ab_heard(p) return table.concat(p.heard, ' ') end
        function __osc_ab_effects(p)
            local n = 0
            for _ in p.acu.BuildEffectsBag do n = n + 1 end
            return n
        end
    )")) {
        return;
    }
    ticks(60);
    run("both guard their upgrading factory", R"(
        IssueGuard({__osc_ab_moved.acu}, __osc_ab_moved.fac)
        IssueGuard({__osc_ab_done.acu}, __osc_ab_done.fac)
    )");
    const u32 moved = global_number("__osc_ab_moved_id");
    const u32 done = global_number("__osc_ab_done_id");
    if (!until("both begin to help", "__osc_ab_heard(__osc_ab_moved) == 'OnStartBuild' and "
                                     "__osc_ab_heard(__osc_ab_done) == 'OnStartBuild'")) {
        return;
    }
    ticks(2);
    t.check(drawn_beams(moved) > 0, "1: the helping commander's beams are drawn");
    run("1: its build effects stand",
        "if __osc_ab_effects(__osc_ab_moved) == 0 then error('none') end");

    run("a Move to one of them, mid-upgrade", R"(
        IssueClearCommands({__osc_ab_moved.acu})
        IssueMove({__osc_ab_moved.acu}, {258, GetTerrainHeight(258, 800), 800})
    )");
    ticks(2);
    run("2: the Move ended its help with one stop, and its effects are gone", R"(
        if __osc_ab_heard(__osc_ab_moved) ~= 'OnStartBuild OnStopBuild' then
            error('it heard "' .. __osc_ab_heard(__osc_ab_moved) .. '"')
        end
        if __osc_ab_effects(__osc_ab_moved) ~= 0 then error('effects remain') end
    )");
    t.check(drawn_beams(moved) == 0, "2: the next frame draws none of its beams");
    run("2: the other commander, still helping, heard no stop",
        "if __osc_ab_heard(__osc_ab_done) ~= 'OnStartBuild' then "
        "error(__osc_ab_heard(__osc_ab_done)) end");

    if (!until("the other's upgrade finishes, its old factory gone",
               "__osc_ab_done.fac:BeenDestroyed()")) {
        return;
    }
    ticks(2);
    run("3: its help ended with the upgrade: one stop heard, effects gone", R"(
        if __osc_ab_heard(__osc_ab_done) ~= 'OnStartBuild OnStopBuild' then
            error('it heard "' .. __osc_ab_heard(__osc_ab_done) .. '"')
        end
        if __osc_ab_effects(__osc_ab_done) ~= 0 then error('effects remain') end
    )");
    t.check(drawn_beams(done) == 0, "3: the next frame draws none of its beams");
    spdlog::info("Assist beam test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
