// --binding-tail-test: engine calls from retail's scripts that the binding
// coverage ratchet listed as missing (M184's tail), as Moho answers them.
//
// On dry ground away from the starts: ARMY_1's platoon of a transport (in
// the scout squad, named in lower case as retail's landing AI names it), a
// marine (attack) and a second transport left unassigned, and a ferry
// beacon; ARMY_2's two power generators 60 apart, which ARMY_1 scries.
// Platoon transport orders go to squads Attack to Scout only;
// FlushIntelInRect makes ARMY_1 forget the generator in its rect (and what
// it saw of it) but not the other; PickBestAttackVector chooses among the
// brain's attack vectors.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>

namespace osc::test {

void test_binding_tail(TestContext& ctx) {
    spdlog::info("=== BINDING TAIL TEST: retail's last missing engine calls ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const auto check = [&](const std::string& what, const std::string& code) {
        const auto ran = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(ran), ran ? what : what + ": " + ran.error().message);
    };
    const auto first_order = [&](u32 id) -> std::optional<sim::CommandType> {
        const auto* e = ctx.sim.entity_registry().find(id);
        if (!e || !e->is_unit()) return std::nullopt;
        const auto& queue = static_cast<const sim::Unit*>(e)->command_queue();
        if (queue.empty()) return std::nullopt;
        return queue.front().type;
    };
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };

    const u32 transport = spawn_unit(ctx, "__osc_bt_transport", "uea0107", "ARMY_1", {sx, sz});
    const u32 marine = spawn_unit(ctx, "__osc_bt_marine", "uel0106", "ARMY_1", {sx + 6, sz});
    const u32 idle = spawn_unit(ctx, "__osc_bt_idle", "uea0107", "ARMY_1", {sx - 8, sz});
    (void)spawn_unit(ctx, "__osc_bt_beacon", "ueb5102", "ARMY_1", {sx + 20, sz - 30});
    const u32 near_gen = spawn_unit(ctx, "__osc_bt_near", "ueb1101", "ARMY_2", {sx + 40, sz + 60});
    (void)spawn_unit(ctx, "__osc_bt_far", "ueb1101", "ARMY_2", {sx - 20, sz + 60});
    check("setup: a platoon, squads named in any case", R"(
        local brain = ArmyBrains[1]
        __osc_bt_platoon = brain:MakePlatoon('binding tail', 'none')
        brain:AssignUnitsToPlatoon(__osc_bt_platoon, {__osc_bt_transport}, 'scout', 'None')
        brain:AssignUnitsToPlatoon(__osc_bt_platoon, {__osc_bt_marine}, 'Attack', 'None')
        brain:AssignUnitsToPlatoon(__osc_bt_platoon, {__osc_bt_idle}, 'Unassigned', 'None')
        for _, name in {'Scout', 'SCOUT', 'scout'} do
            local n = table.getn(__osc_bt_platoon:GetSquadUnits(name))
            if n ~= 1 then error(name .. ' squad has ' .. n) end
        end
        if not __osc_bt_platoon:GetSquadPosition('attack') then error('no attack squad') end
    )");

    // UnloadAllAtLocation: the scout squad's transport takes one unload
    // order; the unassigned one, and the marine, none.
    check("UnloadAllAtLocation orders the squads' transports", fmt::format(R"(
        __osc_bt_unload = __osc_bt_platoon:UnloadAllAtLocation({{{}, 0, {}}})
        if type(__osc_bt_unload) ~= 'number' then error('no command: ' .. tostring(__osc_bt_unload)) end
        if not __osc_bt_platoon:IsCommandsActive(__osc_bt_unload) then error('not active') end
        if pcall(__osc_bt_platoon.UnloadAllAtLocation, __osc_bt_platoon, {{0/0, 0, 0}}) then
            error('a point that is not one was taken')
        end
    )",
                                                                           sx + 40, sz));
    t.check(first_order(transport) == sim::CommandType::TransportUnload,
            "the scout squad's transport unloads");
    t.check(!first_order(idle), "the unassigned transport has no order");
    t.check(!first_order(marine), "the marine has no unload order");

    // UseFerryBeacon: the marine (LAND) waits at the beacon; air stays.
    check("UseFerryBeacon sends the category to the beacon", R"(
        local cmd = __osc_bt_platoon:UseFerryBeacon(categories.LAND, __osc_bt_beacon)
        if type(cmd) ~= 'number' then error('no command: ' .. tostring(cmd)) end
        if __osc_bt_platoon:UseFerryBeacon(categories.NAVAL, __osc_bt_beacon) then
            error('a command for no unit')
        end
    )");
    t.check(first_order(marine) == sim::CommandType::WaitForFerry,
            "the marine waits for a ferry at the beacon");

    // FlushIntelInRect. ARMY_1 scries both generators; OnIntelChange logged.
    check("intel log", R"(
        __osc_bt_log = {}
        local brain = ArmyBrains[1]
        local original = brain.OnIntelChange
        brain.OnIntelChange = function(self, blip, recon, val)
            table.insert(__osc_bt_log, {id = tonumber(blip:GetEntityId()), recon = recon, val = val})
            if original then return original(self, blip, recon, val) end
        end
        function __osc_bt_logged(unit, recon, val)
            local id = tonumber(unit:GetEntityId())
            for _, e in __osc_bt_log do
                if e.id == id and e.recon == recon and e.val == val then return true end
            end
            return false
        end
    )");
    bool scrying = true;
    const auto next = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            if (scrying)
                check("scry", fmt::format("CreateVisibleAreaAtPoint(1, {0}, 0, {1}, 8, 0.1)\n"
                                          "CreateVisibleAreaAtPoint(1, {2}, 0, {1}, 8, 0.1)\n",
                                          sx + 40, sz + 60, sx - 20));
            ctx.sim.tick();
        }
    };
    next(3);
    check("both generators are in sight", R"(
        for _, u in {__osc_bt_near, __osc_bt_far} do
            local blip = u:GetBlip(1)
            if not blip or not blip:IsSeenEver(1) then error('not seen') end
        end
    )");
    const std::string flush = fmt::format("__osc_bt_log = {{}}\nFlushIntelInRect({}, {}, {}, {})\n",
                                          sx + 30, sz + 50, sx + 50, sz + 70);
    check("a flush in sight loses what was seen, and says so", flush + R"(
        if __osc_bt_near:GetBlip(1):IsSeenEver(1) then error('still seen ever') end
        if not __osc_bt_logged(__osc_bt_near, 'LOSNow', false) then error('no LOSNow lost') end
        if not __osc_bt_far:GetBlip(1):IsSeenEver(1) then error('the other forgotten too') end
    )");
    const auto& events = ctx.sim.intel_flush_events();
    t.check(events.size() == 1 && events[0].forgotten.size() == 1 &&
                events[0].forgotten[0].first == near_gen && (events[0].forgotten[0].second & 1u),
            "the flush is handed on for the renderer: army 1 forgot the near generator");
    next(1);
    t.check(ctx.sim.intel_flush_events().empty(), "the tick's flushes are forgotten after it");
    check("still in sight, it is seen again", R"(
        if not __osc_bt_logged(__osc_bt_near, 'LOSNow', true) then error('no LOSNow again') end
        if not __osc_bt_near:GetBlip(1):IsSeenEver(1) then error('not seen ever') end
    )");
    scrying = false;
    run(10);
    check("out of sight, both are remembered", R"(
        for _, u in {__osc_bt_near, __osc_bt_far} do
            local blip = u:GetBlip(1)
            if not blip or not blip:IsSeenEver(1) then error('forgotten') end
        end
    )");
    check("a flush out of sight forgets the one in the rect", flush + R"(
        if __osc_bt_near:GetBlip(1) then error('the near generator has a blip') end
        if table.getn(__osc_bt_log) ~= 0 then error('intel changed out of sight') end
        local far = __osc_bt_far:GetBlip(1)
        if not far or not far:IsSeenEver(1) then error('the far generator forgotten') end
    )");
    check("FlushIntelInRect takes four numbers", R"(
        if pcall(FlushIntelInRect, 1, 2, 3) then error('three taken') end
        if pcall(FlushIntelInRect, 1, 2, 3, 'x') then error('a string taken') end
    )");

    // PickBestAttackVector over ARMY_2's structures (the generators among
    // them): one of the vectors, the closest no farther than the furthest;
    // HighestValue with nothing to value falls back on Closest.
    check("PickBestAttackVector", R"(
        local brain = ArmyBrains[1]
        local enemy = brain:GetCurrentEnemy()
        brain:SetCurrentEnemy(ArmyBrains[2])
        brain:SetUpAttackVectorsToArmy(categories.STRUCTURE)
        local vecs = brain:GetAttackVectors()
        local p = __osc_bt_platoon
        local closest = brain:PickBestAttackVector(p, 'Attack', 'Enemy', 'Closest', nil)
        local furthest = brain:PickBestAttackVector(p, 'attack', 'ENEMY', 'COMPARE_Furthest', nil)
        local valued = brain:PickBestAttackVector(p, 'Attack', 'Enemy', 'HighestValue', nil)
        local guard = brain:PickBestAttackVector(p, 'Guard', 'Enemy', 'Closest', nil)
        local bad = pcall(brain.PickBestAttackVector, brain, p, 'Attack', 'Enemy', 'Sideways', nil)
        brain:SetCurrentEnemy(enemy)
        if not vecs or table.getn(vecs) == 0 then error('no attack vectors') end
        if not closest or not furthest then error('no vector picked') end
        local function listed(v)
            for _, w in vecs do
                if w.px == v.px and w.pz == v.pz and w.vx == v.vx and w.vz == v.vz then return true end
            end
            return false
        end
        if not listed(closest) or not listed(furthest) then error('a vector not listed') end
        local at = p:GetSquadPosition('Attack')
        local function d(v)
            local x, z = v.px + v.vx - at[1], v.pz + v.vz - at[3]
            return x * x + z * z
        end
        if d(closest) > d(furthest) then error('closest is farther than furthest') end
        if not valued or valued.px ~= closest.px or valued.pz ~= closest.pz then
            error('nothing valued did not fall back on closest')
        end
        if guard then error('a vector for an empty squad') end
        if bad then error('an unknown compare type taken') end
    )");
    check("GetAttackVectors is nil when there are none", R"(
        local brain = ArmyBrains[2]
        brain:SetUpAttackVectorsToArmy(categories.STRUCTURE)
        if brain:GetCurrentEnemy() == nil and brain:GetAttackVectors() ~= nil then
            error('vectors without an enemy')
        end
    )");

    const auto queued = [&](u32 id) {
        const auto* e = ctx.sim.entity_registry().find(id);
        return e && e->is_unit() ? static_cast<const sim::Unit*>(e)->command_queue().size() : 0;
    };
    check("a platoon's move for one squad", fmt::format(R"(
        __osc_bt_platoon:Stop()
        __osc_bt_platoon:MoveToLocation({{{0}, 0, {1}}}, false, 'Scout')
    )",
                                                        sx, sz + 20));
    t.check(first_order(transport) == sim::CommandType::Move, "the scout squad moves");
    t.check(!first_order(marine), "the attack squad has no order");
    t.check(!first_order(idle), "the unassigned transport has no order");
    check("an attack-move for another squad, then a move for every squad",
          fmt::format(R"(
        __osc_bt_platoon:AggressiveMoveToLocation({{{0}, 0, {1}}}, 'attack')
        __osc_bt_platoon:MoveToLocation({{{0}, 0, {2}}}, false)
        if pcall(__osc_bt_platoon.Patrol, __osc_bt_platoon, {{{0}, 0, {1}}}, 'Scouts') then
            error('an unknown squad taken')
        end
    )",
                      sx, sz + 20, sz + 30));
    t.check(first_order(marine) == sim::CommandType::AggressiveMove && queued(marine) == 2,
            "the attack squad attack-moves, then moves");
    t.check(queued(transport) == 2, "the scout squad moves twice");
    t.check(!first_order(idle), "the unassigned transport still has no order");

    spdlog::info("=== BINDING TAIL TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
