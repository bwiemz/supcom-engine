// --capture-team-test: engineers capturing one unit together, as the
// original game's CUnitCaptureTask has it (faf-re 0x006043E0, the same code
// at the Steam exe's 0x0060AEB0).
//
// Each engineer's task asks its own script what the capture costs
// (Unit:GetCaptureCosts: half the target's BuildTime over the engineer's
// build rate, times its CaptureTimeMultiplier, and the target's
// BuildCostEnergy), for its target and each unit attached to it, then asks
// its army for its share a tick. Each tick it is paid, its progress goes up
// by the target's count of captors. T1 engineers (UEL0105, build rate 5)
// capture enemy T1 tanks (UEL0201: BuildTime 280, 266 energy):
// 1. one, two and three engineers take 280, 140 and 94 ticks, and spend 266,
//    266 and 267.9 energy between them;
// 2. one of two stopping leaves the tank being captured, and the other goes
//    on alone: a tick of progress a tick;
// 3. when one completes, the others hear OnStopCapture: in its tick if they
//    complete too, else the next, with themselves for the target (Moho's
//    task finds its target gone); just one new tank is made;
// 4. the engineer's script sets the time: CaptureTimeMultiplier 0.5 takes
//    140 ticks;
// 5. a unit attached to the target adds its own costs: a tank carrying a
//    tank takes 560 ticks and 532 energy;
// 6. an aircraft (on the Air layer) can't be captured: the order ends at
//    once, before it starts;
// 7. a unit being captured can't be joined from more than 10 off: that
//    engineer's order ends at once, where it stands.
// Before, each engineer worked alone at the full cost (three took 280 ticks
// and 796 energy), the others heard nothing, the first to stop cleared
// BeingCaptured for all, the script's multiplier and the attached tank
// counted for nothing, and the scout and the far helper were let in.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace osc::test {

namespace {

/// What a scene's engineer was paid for its capture.
struct Paid {
    sim::EntityId id{};
    int ticks = 0;
    f64 energy = 0.0;
};

} // namespace

void test_capture_team(TestContext& ctx) {
    spdlog::info("=== CAPTURE TEAM TEST: engineers capture a tank together ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](sim::EntityId id) {
        sim::Entity* e = id ? registry.find(id) : nullptr;
        return e && e->is_unit() && !e->destroyed() ? static_cast<sim::Unit*>(e) : nullptr;
    };
    const auto lua_check = [&](const std::string& what, const std::string& code) {
        const auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r), r ? what : fmt::format("{}: {}", what, r.error().message));
    };
    const auto number = [&](const std::string& expr) {
        const auto r = ctx.lua_state.do_string("__ct_value = " + expr);
        if (!r) return -1.0;
        lua_pushstring(ctx.L, "__ct_value");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const f64 v = lua_isnumber(ctx.L, -1) ? lua_tonumber(ctx.L, -1) : -1.0;
        lua_pop(ctx.L, 1);
        return v;
    };
    // The entity id of the unit `expr` names.
    const auto entity = [&](const std::string& expr) {
        if (!ctx.lua_state.do_string("__ct_value = tonumber((" + expr + "):GetEntityId())")) {
            return sim::EntityId{};
        }
        lua_pushstring(ctx.L, "__ct_value");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const sim::EntityId id = osc::sim::to_entity_id(ctx.L, -1, registry);
        lua_pop(ctx.L, 1);
        return id;
    };

    // Every capture callback, as heard: its tick, who heard it, and who it
    // names (by tag).
    run_lua(ctx, fmt::format("__ct_x0, __ct_z0 = {}, {}", spot->x, spot->z));
    const auto setup = ctx.lua_state.do_string(R"(
        local brain = GetArmyBrain('ARMY_1')
        brain:GiveStorage('ENERGY', 500000)
        brain:GiveResource('ENERGY', 500000)
        __ct_log = {}
        local heard = {'OnStartCapture', 'OnStopCapture', 'OnFailedCapture', 'OnStartBeingCaptured',
                       'OnStopBeingCaptured', 'OnFailedBeingCaptured', 'OnCaptured'}
        function __ct_watch(u, tag)
            u.__ct_tag = tag
            for _, name in heard do
                local base, n = u[name], name
                u[n] = function(self, other)
                    table.insert(__ct_log, {tick = GetGameTick(), tag = tag, what = n,
                                             other = other and (other.__ct_tag or '?') or 'nil'})
                    if base then return base(self, other) end
                end
            end
        end
        function __ct_events(tag, what)
            local out = {}
            for _, e in __ct_log do
                if e.tag == tag and e.what == what then table.insert(out, e) end
            end
            return out
        end
        function __ct_first(tag, what)
            local e = __ct_events(tag, what)[1]
            return e and e.tick or -1
        end
        local function spawn(bp, army, x, z)
            return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, 0, 0)
        end
        __ct = {}
        -- A scene: an enemy `bp` at (x, z), and engineers beside it.
        function __ct_scene(name, x, z, offsets, bp)
            local s = {x = x, z = z, e = {}}
            s.t = spawn(bp or 'uel0201', 'ARMY_2', x, z)
            s.t:SetFireState(1)
            -- Held fire for the new one too, beside the other scenes' tanks
            s.t:AddUnitCallback(function(new) new:SetFireState(1) end, 'OnCapturedNewUnit')
            __ct_watch(s.t, name .. '.t')
            for i, o in offsets do
                s.e[i] = spawn('uel0105', 'ARMY_1', x + o[1], z + o[2])
                __ct_watch(s.e[i], name .. '.e' .. i)
            end
            __ct[name] = s
        end
        local x0, z0 = __ct_x0, __ct_z0
        local beside = {{3, 0}, {-3, 0}, {0, 3}}
        __ct_scene('one', x0 - 20, z0 - 30, {beside[1]})
        __ct_scene('two', x0, z0 - 30, {beside[1], beside[2]})
        __ct_scene('three', x0 + 20, z0 - 30, beside)
        __ct_scene('quit', x0 + 40, z0 - 30, {beside[1], beside[2]})
        __ct_scene('slow', x0 - 20, z0 + 10, {beside[1]})
        __ct_scene('rider', x0, z0 + 10, {beside[1]})
        __ct_scene('late', x0 + 20, z0 + 10, {beside[1], beside[2]})
        __ct_scene('air', x0 + 40, z0 + 10, {beside[1]}, 'uea0101')
        __ct_scene('far', x0 - 10, z0 + 50, {beside[1], {20, 0}})
        -- 4: the engineer's own multiplier; 5: a tank attached to the tank
        __ct.slow.e[1]:SetCaptureTimeMultiplier(0.5)
        __ct.rider.r = spawn('uel0201', 'ARMY_2', x0 + 1, z0 + 10)
        __ct.rider.r:SetFireState(1)
        __ct.rider.r:AttachTo(__ct.rider.t, 0)
    )");
    t.check(static_cast<bool>(setup),
            setup ? std::string("setup: nine scenes, engineers beside enemy units")
                  : fmt::format("setup: {}", setup.error().message));
    if (!setup) return;
    lua_check("setup: the scout is in the air", R"(
        local layer = __ct.air.t:GetCurrentLayer()
        if layer ~= 'Air' then error('on ' .. tostring(layer)) end
    )");

    // Each engineer, by scene, and what it is paid a tick.
    const std::vector<std::pair<std::string, int>> scenes = {{"one", 1},  {"two", 2},  {"three", 3},
                                                             {"quit", 2}, {"slow", 1}, {"rider", 1},
                                                             {"late", 2}, {"air", 1},  {"far", 2}};
    std::map<std::string, std::vector<Paid>> paid;
    for (const auto& [name, count] : scenes) {
        for (int i = 1; i <= count; ++i) {
            paid[name].push_back({entity(fmt::format("__ct.{}.e[{}]", name, i)), 0, 0.0});
        }
    }
    bool found = true;
    for (const auto& [name, engineers] : paid) {
        for (const Paid& p : engineers) found = found && unit(p.id) != nullptr;
    }
    t.check(found, "setup: every engineer found");
    const auto account = [&] {
        for (auto& [name, engineers] : paid) {
            for (Paid& p : engineers) {
                const sim::Unit* u = unit(p.id);
                const f64 spent =
                    u ? u->economy().spent_energy * sim::SimState::SECONDS_PER_TICK : 0.0;
                if (spent > 0.0) {
                    ++p.ticks;
                    p.energy += spent;
                }
            }
        }
    };
    const auto total = [&](const std::string& name) {
        f64 sum = 0.0;
        for (const Paid& p : paid[name]) sum += p.energy;
        return sum;
    };

    run_lua(ctx, R"(
        for _, name in {'one', 'two', 'three', 'quit', 'slow', 'rider', 'late', 'air'} do
            local s = __ct[name]
            IssueCapture(name == 'late' and {s.e[1]} or s.e, s.t)
        end
        IssueCapture({__ct.far.e[1]}, __ct.far.t)
    )");

    // 2: the second quits after 40 paid ticks; 3: the late one joins after
    // 20; 7: the far one is sent after 10, and should end where it stands.
    bool quit_sent = false;
    bool late_sent = false;
    bool far_sent = false;
    bool quit_held = false;
    sim::Vector3 far_from{};
    const char* captured_all =
        "__ct_first('one.t', 'OnCaptured') >= 0 and __ct_first('two.t', 'OnCaptured') >= 0 and "
        "__ct_first('three.t', 'OnCaptured') >= 0 and __ct_first('quit.t', 'OnCaptured') >= 0 and "
        "__ct_first('slow.t', 'OnCaptured') >= 0 and __ct_first('rider.t', 'OnCaptured') >= 0 and "
        "__ct_first('late.t', 'OnCaptured') >= 0 and __ct_first('far.t', 'OnCaptured') >= 0 and "
        "__ct_first('late.e2', 'OnStopCapture') >= 0";
    tick_until(ctx, "every scene's tank is captured", 800, [&] {
        account();
        if (!quit_sent && paid["quit"][1].ticks >= 40) {
            quit_sent = true;
            run_lua(ctx, "IssueClearCommands({__ct.quit.e[2]})");
            quit_held = lua_true(ctx.lua_state, "__ct.quit.t:IsUnitState('BeingCaptured')");
        }
        if (!late_sent && paid["late"][0].ticks >= 20) {
            late_sent = true;
            run_lua(ctx, "IssueCapture({__ct.late.e[2]}, __ct.late.t)");
        }
        if (!far_sent && paid["far"][0].ticks >= 10) {
            far_sent = true;
            if (const sim::Unit* u = unit(paid["far"][1].id)) far_from = u->position();
            run_lua(ctx, "IssueCapture({__ct.far.e[2]}, __ct.far.t)");
        }
        return lua_true(ctx.lua_state, captured_all);
    });

    // 1. The time from the start to the capture, and the energy, by team.
    const auto took = [&](const std::string& name) {
        return number(fmt::format("__ct_first('{0}.t', 'OnCaptured') - __ct_first('{0}.e1', "
                                  "'OnStartCapture')",
                                  name));
    };
    const struct {
        const char* name;
        f64 ticks;
        f64 energy;
    } teams[] = {{"one", 280, 266.0}, {"two", 140, 266.0}, {"three", 94, 267.9}};
    for (const auto& team : teams) {
        t.check(took(team.name) == team.ticks && std::abs(total(team.name) - team.energy) < 0.05,
                fmt::format("1: {} engineer(s) capture the tank in {} ticks, spending {:.1f} "
                            "energy (in {} ticks, spending {:.2f})",
                            paid[team.name].size(), team.ticks, team.energy, took(team.name),
                            total(team.name)));
    }

    // 2. One of two stops after 40 ticks: the tank is still being captured,
    // the one that stopped failed, and the other went on alone.
    const f64 quit_expect = 280.0 - paid["quit"][1].ticks;
    t.check(quit_held, "2: the tank is still BeingCaptured after one of its two captors stops");
    lua_check("2: the one that stopped failed its capture, and the tank heard it", R"(
        if table.getn(__ct_events('quit.e2', 'OnFailedCapture')) ~= 1 then
            error('the engineer heard no OnFailedCapture')
        end
        local heard = __ct_events('quit.t', 'OnFailedBeingCaptured')
        if table.getn(heard) ~= 1 or heard[1].other ~= 'quit.e2' then
            error('the tank heard no OnFailedBeingCaptured from it')
        end
    )");
    t.check(took("quit") == quit_expect,
            fmt::format("2: the other captures it alone from there: in {} ticks ({} paid "
                        "together) ({})",
                        quit_expect, paid["quit"][1].ticks, took("quit")));

    // 3. The others hear OnStopCapture, and one tank is made.
    lua_check("3: completing together, each of the team hears OnStopCapture once, in that tick",
              R"(
        for _, s in {{'two', 2}, {'three', 3}} do
            local at = __ct_first(s[1] .. '.t', 'OnCaptured')
            for i = 1, s[2] do
                local heard = __ct_events(s[1] .. '.e' .. i, 'OnStopCapture')
                if table.getn(heard) ~= 1 or heard[1].tick ~= at then
                    error(s[1] .. '.e' .. i .. ' heard OnStopCapture ' .. table.getn(heard) ..
                          ' time(s), first at ' .. tostring(heard[1] and heard[1].tick) .. ', not ' .. at)
                end
            end
        end
    )");
    lua_check("3: one behind hears OnStopCapture in the next tick, itself the target, and fails "
              "nothing",
              R"(
        local at = __ct_first('late.t', 'OnCaptured')
        local heard = __ct_events('late.e2', 'OnStopCapture')
        if table.getn(heard) ~= 1 then error(table.getn(heard) .. ' OnStopCapture') end
        if heard[1].tick ~= at + 1 then error('at ' .. heard[1].tick .. ', captured at ' .. at) end
        if heard[1].other ~= 'late.e2' then error('naming ' .. heard[1].other) end
        if table.getn(__ct_events('late.e2', 'OnFailedCapture')) ~= 0 then
            error('it heard OnFailedCapture')
        end
    )");
    // 20 alone, then 2 a tick: 150 ticks, or 151 if its own task runs before
    // the helper's starts in the helper's first tick
    t.check(took("late") == 150 || took("late") == 151,
            fmt::format("3: the first, helped after 20 ticks, takes 150 ({})", took("late")));
    lua_check("3: each team makes one tank of ARMY_1", R"(
        for _, name in {'one', 'two', 'three', 'quit', 'slow', 'late', 'far'} do
            local s = __ct[name]
            local n = 0
            for _, u in GetUnitsInRect(s.x - 2, s.z - 2, s.x + 2, s.z + 2) or {} do
                if u:GetArmy() == 1 and u:GetUnitId() == 'uel0201' and not u:IsDead() then
                    n = n + 1
                end
            end
            if n ~= 1 then error(name .. ': ' .. n) end
        end
    )");

    // 4. The engineer's script decides the time.
    t.check(took("slow") == 140,
            fmt::format("4: CaptureTimeMultiplier 0.5 takes 140 ticks ({})", took("slow")));

    // 5. The attached tank adds its own costs.
    t.check(took("rider") == 560 && std::abs(total("rider") - 532.0) < 0.05,
            fmt::format("5: a tank carrying a tank takes 560 ticks and 532 energy ({}, {:.2f})",
                        took("rider"), total("rider")));

    // 6. An aircraft can't be captured.
    const sim::Unit* air = unit(paid["air"][0].id);
    t.check(air && !air->is_capturing() && air->command_queue().empty() &&
                paid["air"][0].ticks == 0,
            "6: the engineer's capture of a flying scout ends at once, unpaid");
    lua_check("6: nothing started, and the scout is ARMY_2's still", R"(
        if __ct_first('air.e1', 'OnStartCapture') >= 0 then error('it started') end
        if IsDestroyed(__ct.air.t) or __ct.air.t:GetArmy() ~= 2 then error('taken') end
    )");

    // 7. The far helper ends its order where it stands.
    const sim::Unit* helper = unit(paid["far"][1].id);
    const f32 moved =
        helper ? std::hypot(helper->position().x - far_from.x, helper->position().z - far_from.z)
               : -1.0f;
    t.check(far_sent && helper && helper->command_queue().empty() && moved >= 0.0f &&
                moved < 0.5f && paid["far"][1].ticks == 0,
            fmt::format("7: an engineer sent from 18 off to a tank being captured ends its order "
                        "where it stands (moved {:.1f}, paid {} ticks)",
                        moved, paid["far"][1].ticks));
    lua_check("7: it never started", R"(
        if __ct_first('far.e2', 'OnStartCapture') >= 0 then error('it started') end
    )");
    t.check(took("far") == 280,
            fmt::format("7: the first went on alone: 280 ticks ({})", took("far")));
    spdlog::info("=== CAPTURE TEAM TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
