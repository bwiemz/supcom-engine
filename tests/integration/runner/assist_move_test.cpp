// --transport-assist-move-test: a Transport order given to air transports with the
// land units they are to carry (Moho's UNITCOMMAND_AssistMove).
//
// Moho's command mode, given the Transport order on the ground with air
// transports and land units selected, gives them all one AssistMove there
// (SCommandModeData::HandleEvent's RULEUCC_Transport arm). The land units
// set off; each transport (CUnitAssistMoveTask) picks up the ones that get
// there sooner carried than driven (Unit::NeedsPickup), sets them down at the
// goal, and goes on until none is left to carry. A land unit's order ends as
// it boards; a transport's at the goal.
//
// The scene: two UEF T1 transports and six UEF T1 tanks on the west bank of
// the strait at z 520 on Seton's Clutch (tools/moho_probes `shore`: a tank
// drives some 500 round it), ordered to (392, 520) across it. Before, only
// the transports took the order, as an unload with nothing aboard that ended
// at once, and the tanks got none.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "renderer/input_handler.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::test {

namespace {

void lua_check(TestContext& ctx, Tally& t, const char* what, const char* code) {
    const auto r = ctx.lua_state.do_string(code);
    t.check(static_cast<bool>(r),
            r ? std::string(what) : fmt::format("{}: {}", what, r.error().message));
}

} // namespace

void test_assist_move(TestContext& ctx) {
    spdlog::info("=== ASSIST MOVE TEST: transports carry the land units ordered with them ===");
    Tally t;
    const int failures_before = osc::test_status::failure_count();

    // The other armies' commanders start on the map: neutral to ARMY_1, none
    // fights the scene.
    lua_check(ctx, t, "setup: the scene", R"(
        for _, army in ListArmies() do
            if army ~= 'ARMY_1' then SetAlliance('ARMY_1', army, 'Neutral') end
        end
        local function spawn(bp, x, z)
            return CreateUnitHPR(bp, 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
        end
        __osc_assist = {tanks = {}, transports = {}, ids = {}, goal = {392, 520}}
        for i = 0, 5 do
            local tank = spawn('uel0201', 158 + 4 * math.mod(i, 3), 516 + 4 * math.floor(i / 3))
            table.insert(__osc_assist.tanks, {unit = tank, layers = {}})
            table.insert(__osc_assist.ids, tonumber(tank:GetEntityId()))
        end
        for i = 0, 1 do
            local transport = spawn('uea0107', 146, 510 + 20 * i)
            table.insert(__osc_assist.transports, {unit = transport})
            table.insert(__osc_assist.ids, tonumber(transport:GetEntityId()))
        end
        function __osc_assist_sample(tick)
            for _, s in __osc_assist.tanks do
                if not s.unit:IsDead() then
                    if s.unit:IsUnitState('Attached') then s.carried = s.carried or tick end
                    if not s.unit:IsUnitState('Attached') then
                        s.layers[s.unit:GetCurrentLayer()] = true
                    end
                end
            end
            for _, s in __osc_assist.transports do
                if not s.unit:IsDead() then
                    if s.unit:IsUnitState('AssistMoving') then s.assisting = true end
                    s.most = math.max(s.most or 0, table.getn(s.unit:GetCargo()))
                end
            end
        end
    )");

    // All eight selected, the Transport order clicked on the far bank.
    std::unordered_set<sim::EntityId> selected;
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_assist");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "ids");
        lua_rawget(L, -2);
        for (int i = 1; lua_istable(L, -1) && i <= 8; ++i) {
            lua_rawgeti(L, -1, i);
            const sim::EntityId id = sim::to_entity_id(L, -1, ctx.sim.entity_registry());
            if (id) selected.insert(id);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    t.check(selected.size() == 8, fmt::format("setup: 8 units selected ({})", selected.size()));
    renderer::InputHandler input;
    input.set_selected(selected);
    renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Transport";
    const auto issued = input.click_in_command_mode(ctx.sim, mode, 392.0f, 520.0f, false);
    t.check(issued && issued->type == "AssistMove" && issued->units.size() == 8,
            fmt::format("1: the Transport order is one AssistMove for all eight ({}, {} units)",
                        issued ? issued->type : std::string("none"),
                        issued ? issued->units.size() : 0));
    tick_for(ctx, 2);
    lua_check(ctx, t, "1: ... in each one's queue (UNITCOMMAND_AssistMove, 37)", R"(
        local function check(u)
            local q = u:GetCommandQueue()
            if table.getn(q) ~= 1 or q[1].commandType ~= 37 then
                error(u:GetBlueprint().BlueprintId .. ': ' .. table.getn(q) .. ' orders, the first ' ..
                      tostring(q[1] and q[1].commandType))
            end
        end
        for _, s in __osc_assist.tanks do check(s.unit) end
        for _, s in __osc_assist.transports do check(s.unit) end
    )");

    int tick = 2;
    tick_until(
        [&] {
            ctx.sim.tick();
            ++tick;
            if (const auto r =
                    ctx.lua_state.do_string(fmt::format("__osc_assist_sample({})", tick));
                !r) {
                t.check(false, fmt::format("sampling at tick {}: {}", tick, r.error().message));
            }
        },
        "the order ends: every unit's queue empty and none moving", 1500,
        [&] {
            return lua_true(ctx.lua_state, R"((function()
                local all = {}
                for _, s in __osc_assist.tanks do table.insert(all, s.unit) end
                for _, s in __osc_assist.transports do table.insert(all, s.unit) end
                for _, u in all do
                    if not u:IsDead() and (table.getn(u:GetCommandQueue()) > 0 or u:IsMoving() or
                                           u:IsUnitState('Attached')) then
                        return false
                    end
                end
                return true
            end)())");
        });
    spdlog::info("assist-move-test: the order ended at tick {}", tick);
    (void)ctx.lua_state.do_string(R"(
        for i, s in __osc_assist.transports do
            LOG(string.format('assist-move-test: transport %d carried up to %d', i, s.most or 0))
        end
        for i, s in __osc_assist.tanks do
            LOG(string.format('assist-move-test: tank %d aboard from tick %s', i, tostring(s.carried)))
        end
    )");

    lua_check(ctx, t, "2: every tank is carried across, and set down on the far bank", R"(
        local g = __osc_assist.goal
        for i, s in __osc_assist.tanks do
            if s.unit:IsDead() then error('tank ' .. i .. ' died') end
            if not s.carried then error('tank ' .. i .. ' was never carried') end
            local p = s.unit:GetPosition()
            local d = VDist2(p[1], p[3], g[1], g[2])
            if d > 15 then
                error(string.format('tank %d stands %.1f from the goal, at (%.1f, %.1f)', i, d, p[1], p[3]))
            end
            for layer in s.layers do
                if layer ~= 'Land' then error('tank ' .. i .. ' was on the ' .. layer .. ' layer') end
            end
        end
    )");
    lua_check(ctx, t, "3: the transports ran the order, and are done with it at the goal", R"(
        local g = __osc_assist.goal
        for i, s in __osc_assist.transports do
            if not s.assisting then error('transport ' .. i .. ' was never AssistMoving') end
            if s.unit:IsUnitState('AssistMoving') then error('transport ' .. i .. ' is still AssistMoving') end
            if s.unit:IsUnitState('TransportLoading') then error('transport ' .. i .. ' is still loading') end
            local p = s.unit:GetPosition()
            local d = VDist2(p[1], p[3], g[1], g[2])
            if d > 15 then error(string.format('transport %d is %.1f from the goal', i, d)) end
        end
    )");
    const bool all_carried = lua_true(ctx.lua_state, R"((function()
        for _, s in __osc_assist.tanks do
            if not s.carried then return false end
        end
        return true
    end)())");
    t.check(all_carried && tick < 1000,
            fmt::format("4: carried, they are across well before a tank could drive round (all "
                        "carried: {}; the order ended at tick {})",
                        all_carried, tick));
    t.check(osc::test_status::failure_count() - t.fail == failures_before, "5: no script errors");
    spdlog::info("Assist move test: {} passed, {} failed", t.pass, t.fail);
}

// --transport-assist-move-pickups-test: three Transport orders on the west bank
// of Seton's Clutch's strait, 60 ticks apart once the transports have settled.
// A tank bound for the islet off the shore, which no land path reaches; two
// tanks, each farther from its transport than its GuardScanRadius, whose goal
// cell's centre and the point clicked in it lie either side of the distance at
// which a lift pays.
void test_assist_move_pickups(TestContext& ctx) {
    spdlog::info("=== ASSIST MOVE PICKUPS TEST: the Transport order's pickups, as Moho's ===");
    Tally t;
    const int failures_before = osc::test_status::failure_count();
    lua_check(ctx, t, "setup: the scenes", R"(
        for _, army in ListArmies() do
            if army ~= 'ARMY_1' then SetAlliance('ARMY_1', army, 'Neutral') end
        end
        __osc_wait = {ids = {}, heard = {}, states = {}, ordered = {}}
        local function spawn(label, bp, x, z)
            local u = CreateUnitHPR(bp, 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
            __osc_wait[label] = u
            __osc_wait.ids[label] = tonumber(u:GetEntityId())
            for _, name in {'OnStartTransportLoading', 'OnTransportOrdered', 'OnTransportAborted',
                            'OnTransportAttach', 'OnTransportDetach'} do
                local orig = u[name]
                local n = name
                u[n] = function(s, a, b, c)
                    table.insert(__osc_wait.heard, {tick = GetGameTick(), who = label, what = n})
                    if orig then return orig(s, a, b, c) end
                end
            end
        end
        spawn('islet-t', 'uea0107', 175, 620)
        spawn('islet-a', 'uel0201', 190, 612)
        spawn('centre-t', 'uea0107', 102, 612)
        spawn('centre-a', 'uel0201', 72.5, 612.5)
        spawn('point-t', 'uea0107', 102, 640)
        spawn('point-a', 'uel0201', 72.5, 640.5)
        function __osc_wait_sample()
            for _, scene in {'islet', 'centre', 'point'} do
                if not __osc_wait.ordered[scene] and
                   table.getn(__osc_wait[scene .. '-t']:GetCommandQueue()) > 0 then
                    __osc_wait.ordered[scene] = GetGameTick()
                end
            end
            for _, label in {'islet-a'} do
                local u = __osc_wait[label]
                for _, st in {'ProblemGettingToGoal', 'Attached'} do
                    if u:IsUnitState(st) then __osc_wait.states[label .. ' ' .. st] = true end
                end
            end
        end
        function __osc_wait_first(who, what)
            local scene = string.gsub(who, '%-.*', '')
            for _, h in __osc_wait.heard do
                if h.who == who and h.what == what then return h.tick - __osc_wait.ordered[scene] end
            end
        end
    )");
    tick_for(ctx, 580);
    const auto ids = [&](const std::vector<const char*>& labels) {
        std::unordered_set<sim::EntityId> out;
        lua_State* L = ctx.L;
        for (const char* label : labels) {
            lua_pushstring(L, "__osc_wait");
            lua_rawget(L, LUA_GLOBALSINDEX);
            lua_pushstring(L, "ids");
            lua_rawget(L, -2);
            lua_pushstring(L, label);
            lua_rawget(L, -2);
            const sim::EntityId id = sim::to_entity_id(L, -1, ctx.sim.entity_registry());
            lua_pop(L, 3);
            if (id) {
                out.insert(id);
            }
        }
        return out;
    };
    const auto unit = [&](const char* label) -> const sim::Unit* {
        const auto set = ids({label});
        const sim::Entity* e = set.empty() ? nullptr : ctx.sim.entity_registry().find(*set.begin());
        return e && e->is_unit() ? static_cast<const sim::Unit*>(e) : nullptr;
    };
    struct Goal {
        float x = 0.0f;
        float z = 0.0f;
        float margin = -1e9f;
    };
    // Unit::NeedsPickup's break-even distance D* for the tank, and a goal cell
    // with a point in it 0.45 off its centre on each axis, away from the tank
    // (`sign` 1) or towards it (-1): with 1 the point beyond D* and the centre
    // short of it, with -1 the other way.
    const auto plan = [&](const char* tank_label, const char* transport_label, float sign) {
        Goal best;
        const sim::Unit* tank = unit(tank_label);
        const sim::Unit* transport = unit(transport_label);
        if (!tank || !transport) {
            return best;
        }
        const sim::Vector3 p = tank->position();
        const sim::Vector3 q = transport->position();
        const auto dist = [&](float x, float z) {
            return std::sqrt((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z));
        };
        const float speed = tank->max_speed();
        const float lift_speed = transport->max_airspeed();
        const float to_transport = dist(q.x, q.z);
        const bool far = transport->layer() == "Air" || to_transport > tank->guard_scan_radius();
        const float dstar =
            ((far ? 9.0f : 5.0f + to_transport / speed) + to_transport / lift_speed) /
            (1.0f / speed - 1.0f / lift_speed);
        const int n0 = static_cast<int>(std::floor(p.x + dstar));
        const int m0 = static_cast<int>(std::floor(p.z));
        for (int n = n0 - 4; n <= n0 + 4; ++n) {
            for (int m = m0 - 10; m <= m0 + 10; ++m) {
                const float cx = static_cast<float>(n) + 0.5f;
                const float cz = static_cast<float>(m) + 0.5f;
                if (ctx.sim.footprint_fits_at(tank->footprint(), cx, cz) == 0) {
                    continue;
                }
                const float gx = cx + sign * (cx < p.x ? -0.45f : 0.45f);
                const float gz = cz + sign * (cz < p.z ? -0.45f : 0.45f);
                const float margin =
                    std::min((dstar - dist(cx, cz)) * sign, (dist(gx, gz) - dstar) * sign);
                if (margin > best.margin) {
                    best = {gx, gz, margin};
                }
            }
        }
        return best;
    };
    struct Scene {
        std::vector<const char*> pick;
        float x;
        float z;
        float sign;
    };
    const std::vector<Scene> scenes = {{{"islet-t", "islet-a"}, 216.5f, 611.5f, 0.0f},
                                       {{"centre-t", "centre-a"}, 0.0f, 0.0f, -1.0f},
                                       {{"point-t", "point-a"}, 0.0f, 0.0f, 1.0f}};
    renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Transport";
    std::vector<float> margins;
    for (const Scene& sc : scenes) {
        float x = sc.x;
        float z = sc.z;
        if (sc.sign != 0.0f) {
            const Goal g = plan(sc.pick[1], sc.pick[0], sc.sign);
            x = g.x;
            z = g.z;
            margins.push_back(g.margin);
        }
        renderer::InputHandler input;
        input.set_selected(ids(sc.pick));
        const auto issued = input.click_in_command_mode(ctx.sim, mode, x, z, false);
        t.check(issued && issued->type == "AssistMove",
                fmt::format("setup: an AssistMove for {} ({})", sc.pick.front(),
                            issued ? issued->type : std::string("none")));
        for (int i = 0; i < 60; ++i) {
            ctx.sim.tick();
            (void)ctx.lua_state.do_string("__osc_wait_sample()");
        }
    }
    for (int i = 0; i < 600; ++i) {
        ctx.sim.tick();
        (void)ctx.lua_state.do_string("__osc_wait_sample()");
    }
    const auto heard = [&](const char* who, const char* what) {
        lua_State* L = ctx.L;
        const auto r = ctx.lua_state.do_string(
            fmt::format("__osc_wait_got = __osc_wait_first('{}', '{}') or -1", who, what));
        lua_pushstring(L, "__osc_wait_got");
        lua_rawget(L, LUA_GLOBALSINDEX);
        const int v = r ? static_cast<int>(lua_tonumber(L, -1)) : -1;
        lua_settop(L, 0);
        return v;
    };
    const int islet_load = heard("islet-t", "OnStartTransportLoading");
    t.check(lua_true(ctx.lua_state, "__osc_wait.states['islet-a ProblemGettingToGoal'] == true") &&
                lua_true(ctx.lua_state, "__osc_wait.states['islet-a Attached'] == true") &&
                islet_load == 5,
            fmt::format("1: the tank no land path takes to the islet has a problem getting to "
                        "its goal and is picked up (load at T+{})",
                        islet_load));
    const int centre_load = heard("centre-t", "OnStartTransportLoading");
    const int point_load = heard("point-t", "OnStartTransportLoading");
    t.check(margins.size() == 2 && margins[0] > 0.1f && margins[1] > 0.1f && centre_load >= 0 &&
                point_load < 0,
            fmt::format("2: a tank needs a lift by its goal cell's centre, not by the point "
                        "clicked in it (picked up where the centre is the farther: load at "
                        "T+{}; where the point is: {}; margins {})",
                        centre_load, point_load, fmt::join(margins, ", ")));
    t.check(osc::test_status::failure_count() - t.fail == failures_before, "3: no script errors");
    spdlog::info("Assist move pickups test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
