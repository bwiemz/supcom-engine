// --assist-beam-test: a commander helping a factory's upgrade lets its build effects go when
// the help ends -- by a new order, or by the upgrade finishing -- not at its next build
// (retail's OnStopBuild destroys BuildEffectsBag, and the effects leave the frame's snapshot).

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/ieffect.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/world_snapshot.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>
#include <tuple>

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
        const u32 n = osc::sim::to_entity_id(ctx.L, -1);
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
    const auto until = [&](const std::string& what, const std::string& expr,
                           int limit = kWaitTicks) {
        for (int i = 0; i < limit; ++i) {
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
        t.check(false, fmt::format("{}: not within {} ticks", what, limit));
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
    for (const auto& [name, id, var] : {std::tuple{"first", moved, "__osc_ab_moved"},
                                        std::tuple{"second", done, "__osc_ab_done"}}) {
        t.check(drawn_beams(id) > 0, fmt::format("1: the {} commander's beams are drawn", name));
        run(fmt::format("1: the {} commander's build effects stand", name),
            fmt::format("if __osc_ab_effects({}) == 0 then error('none') end", var));
    }
    run("the second upgrade's frame, begun and unfinished", R"(
        __osc_ab_done.t2 = __osc_ab_done.fac:GetFocusUnit()
        if not __osc_ab_done.t2 then error('the factory builds nothing') end
        if __osc_ab_done.t2:GetFractionComplete() >= 1 then error('already finished') end
    )");

    // A player's Move, replacing the queue as the command route applies it.
    {
        sim::UnitCommand move;
        move.type = sim::CommandType::Move;
        move.target_pos = {258.0f, 0.0f, 800.0f};
        ctx.sim.set_human_input_active(true);
        ctx.sim.route_player_command({moved}, move, true);
        ctx.sim.set_human_input_active(false);
    }
    until("the Move ends the first commander's help",
          "__osc_ab_heard(__osc_ab_moved) == 'OnStartBuild OnStopBuild'", 20);
    {
        const auto* acu = static_cast<const sim::Unit*>(ctx.sim.entity_registry().find(moved));
        t.check(acu && !acu->command_queue().empty() &&
                    acu->command_queue().front().type == sim::CommandType::Move,
                "2: the Move was accepted, at the head of its queue");
    }
    run("2: one stop heard, its effects are gone", R"(
        if __osc_ab_heard(__osc_ab_moved) ~= 'OnStartBuild OnStopBuild' then
            error('it heard "' .. __osc_ab_heard(__osc_ab_moved) .. '"')
        end
        if __osc_ab_effects(__osc_ab_moved) ~= 0 then error('effects remain') end
    )");
    t.check(drawn_beams(moved) == 0, "2: the next frame draws none of its beams");
    run("2: the second commander, still helping, heard no stop and still has its effects", R"(
        if __osc_ab_heard(__osc_ab_done) ~= 'OnStartBuild' then error(__osc_ab_heard(__osc_ab_done)) end
        if __osc_ab_effects(__osc_ab_done) == 0 then error('its effects are gone') end
    )");
    t.check(drawn_beams(done) > 0, "2: its beams are still drawn");

    until("the second upgrade is half built", "__osc_ab_done.t2:GetFractionComplete() >= 0.5");
    run("3: halfway, its help goes on: no stop, effects stand", R"(
        if __osc_ab_heard(__osc_ab_done) ~= 'OnStartBuild' then error(__osc_ab_heard(__osc_ab_done)) end
        if __osc_ab_effects(__osc_ab_done) == 0 then error('its effects are gone') end
    )");
    t.check(drawn_beams(done) > 0, "3: halfway, its beams are drawn");
    until("the second upgrade completes", "__osc_ab_done.t2:GetFractionComplete() >= 1");
    until("3: the stop comes within a few ticks of the completion",
          "__osc_ab_heard(__osc_ab_done) == 'OnStartBuild OnStopBuild'", 5);
    run("3: at completion its help ended: one stop heard, effects gone", R"(
        if __osc_ab_heard(__osc_ab_done) ~= 'OnStartBuild OnStopBuild' then
            error('it heard "' .. __osc_ab_heard(__osc_ab_done) .. '"')
        end
        if __osc_ab_effects(__osc_ab_done) ~= 0 then error('effects remain') end
    )");
    t.check(drawn_beams(done) == 0, "3: at completion the frame draws none of its beams");
    spdlog::info("Assist beam test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
