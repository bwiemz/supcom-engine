// --transport-reverse-load-test: the Transport order on a unit that calls transports (Moho's
// UNITCOMMAND_TransportReverseLoadUnits), in the scenes of a probe of the original game on the
// west bank of Seton's Clutch's strait.

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
#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>
#include <unordered_set>

namespace osc::test {

namespace {

void lua_check(TestContext& ctx, Tally& t, const char* what, const std::string& code) {
    const auto r = ctx.lua_state.do_string(code);
    t.check(static_cast<bool>(r),
            r ? std::string(what) : fmt::format("{}: {}", what, r.error().message));
}

sim::EntityId id_of(TestContext& ctx, const char* label) {
    lua_State* L = ctx.L;
    const int top = lua_gettop(L);
    lua_pushstring(L, "__osc_rl_ids");
    lua_rawget(L, LUA_GLOBALSINDEX);
    sim::EntityId id{};
    if (lua_istable(L, -1)) {
        lua_pushstring(L, label);
        lua_rawget(L, -2);
        id = sim::to_entity_id(L, -1, ctx.sim.entity_registry());
    }
    lua_settop(L, top);
    return id;
}

sim::Vector3 position_of(TestContext& ctx, const char* label) {
    const sim::Entity* e = ctx.sim.entity_registry().find(id_of(ctx, label));
    return e ? e->position() : sim::Vector3{};
}

std::string heads(const char* a, const char* b) {
    return fmt::format("__osc_rl_head(__osc_rl['{}']) .. ' ' .. __osc_rl_head(__osc_rl['{}'])", a,
                       b);
}

} // namespace

void test_reverse_load(TestContext& ctx) {
    spdlog::info("=== REVERSE LOAD TEST: the Transport order on a unit that calls transports ===");
    Tally t;
    const int failures_before = osc::test_status::failure_count();

    lua_check(ctx, t, "setup: the transports and the cargo that starts aboard", R"(
        for _, army in ListArmies() do
            if army ~= 'ARMY_1' then SetAlliance('ARMY_1', army, 'Neutral') end
        end
        __osc_rl = {}
        __osc_rl_ids = {}
        function __osc_rl_spawn(label, bp, x, z)
            local u = CreateUnitHPR(bp, 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
            u:SetCanTakeDamage(false)
            __osc_rl[label] = u
            __osc_rl_ids[label] = tonumber(u:GetEntityId())
            return u
        end
        function __osc_rl_head(u)
            local q = u:GetCommandQueue()
            return q[1] and tostring(q[1].commandType) or '-'
        end
        function __osc_rl_aboard(u, t)
            for _, c in t:GetCargo() do
                if c == u then return true end
            end
            return false
        end
        __osc_rl_spawn('load-t', 'uea0107', 160, 556)
        __osc_rl_spawn('pick-ta', 'uea0107', 110, 620)
        __osc_rl_spawn('pick-tb', 'uea0107', 110, 596)
        __osc_rl_spawn('pick-y', 'uel0201', 140, 640)
        __osc_rl_spawn('full-t', 'uea0107', 110, 576)
        local cargo = {}
        for i = 1, 6 do
            table.insert(cargo, __osc_rl_spawn('full-c' .. i, 'uel0201', 96 + 4 * i, 586))
        end
        IssueTransportLoad(cargo, __osc_rl['full-t'])
        __osc_rl_spawn('rclick-t', 'uea0107', 160, 580)
        __osc_rl['carrier'] = CreateUnitHPR('uas0303', 'ARMY_1', 250, GetSurfaceHeight(250, 560), 560, 0, 0, 0)
        __osc_rl_ids['carrier'] = tonumber(__osc_rl['carrier']:GetEntityId())
        __osc_rl_spawn('plane', 'uaa0102', 215, 560)
        __osc_rl_spawn('pad', 'ueb5202', 170, 530)
        __osc_rl_spawn('pad-plane', 'uaa0102', 185, 530)
    )");
    tick_until(ctx, "setup: the full transport takes five tanks aboard and is done loading", 900,
               "table.getn(__osc_rl['full-t']:GetCargo()) >= 5 and "
               "table.getn(__osc_rl['full-t']:GetCommandQueue()) == 0");
    tick_for(ctx, 100);
    lua_check(ctx, t, "setup: the scenes' targets beside their transports", R"(
        local function beside(label, bp, of, dx)
            local p = __osc_rl[of]:GetPosition()
            return __osc_rl_spawn(label, bp, p[1] + dx, p[3])
        end
        beside('load-x', 'uel0201', 'load-t', 16)
        beside('full-x', 'uel0202', 'full-t', 16)
        beside('rclick-x', 'uel0104', 'rclick-t', 16)
        local a = __osc_rl['pick-ta']:GetPosition()
        local b = __osc_rl['pick-tb']:GetPosition()
        local ra, rb = 22, 34
        local dx, dz = b[1] - a[1], b[3] - a[3]
        local d = math.sqrt(dx * dx + dz * dz)
        local along = (ra * ra - rb * rb + d * d) / (2 * d)
        local h = math.sqrt(math.max(ra * ra - along * along, 0))
        local mx, mz = a[1] + along * dx / d, a[3] + along * dz / d
        local px, pz = mx - h * dz / d, mz + h * dx / d
        if mx + h * dz / d > px then
            px, pz = mx + h * dz / d, mz - h * dx / d
        end
        __osc_rl_spawn('pick-x', 'uel0106', px, pz)
        IssueAttack({__osc_rl['pick-y']}, {130, GetTerrainHeight(130, 646), 646})
    )");
    tick_for(ctx, 5);

    renderer::InputHandler input;
    input.set_player_army(0);
    renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Transport";
    const auto click_on = [&](const char* label) {
        const sim::Vector3 at = position_of(ctx, label);
        return input.click_in_command_mode(ctx.sim, mode, at.x, at.z, false);
    };

    input.set_selected({id_of(ctx, "load-t")});
    auto issued = click_on("load-x");
    t.check(issued && issued->type == "TransportReverseLoadUnits" &&
                issued->target_id == id_of(ctx, "load-x"),
            fmt::format("1: a Transport click on the tank is a reverse load at it ({})",
                        issued ? issued->type : std::string("none")));
    tick_for(ctx, 2);
    lua_check(ctx, t, "1: ... in the transport's queue and the tank's (23)",
              "local h = " + heads("load-t", "load-x") + " if h ~= '23 23' then error(h) end");
    tick_until(ctx, "1: the tank boards the transport", 600,
               "__osc_rl_aboard(__osc_rl['load-x'], __osc_rl['load-t'])");
    tick_for(ctx, 1);
    lua_check(ctx, t, "1: aboard, both orders are done",
              "local h = " + heads("load-t", "load-x") + " if h ~= '- -' then error(h) end");

    lua_check(ctx, t, "setup: the nearer transport busy",
              "IssueMove({__osc_rl['pick-ta']}, {60, 0, 900})");
    input.set_selected({id_of(ctx, "pick-ta"), id_of(ctx, "pick-tb"), id_of(ctx, "pick-y")});
    issued = click_on("pick-x");
    t.check(issued && issued->type == "TransportReverseLoadUnits" && issued->units.size() == 3,
            fmt::format("2: the reverse load goes to the three selected ({}, {} units)",
                        issued ? issued->type : std::string("none"),
                        issued ? issued->units.size() : 0));
    tick_for(ctx, 2);
    lua_check(ctx, t,
              "2: the idle transport 34 off takes it, the busy one 22 off and the busy tank "
              "keep their orders",
              "local h = " + heads("pick-tb", "pick-x") + " .. ' ' .. " +
                  "__osc_rl_head(__osc_rl['pick-ta']) .. ' ' .. __osc_rl_head(__osc_rl['pick-y']) "
                  "if h ~= '23 23 2 10' then error(h) end");
    tick_until(ctx, "2: the tank boards the idle transport", 600,
               "__osc_rl_aboard(__osc_rl['pick-x'], __osc_rl['pick-tb'])");

    input.set_selected({id_of(ctx, "full-t")});
    issued = click_on("full-x");
    t.check(issued && issued->type == "TransportReverseLoadUnits",
            fmt::format("3: a Transport click on the T2 tank is a reverse load at it ({})",
                        issued ? issued->type : std::string("none")));
    tick_for(ctx, 2);
    lua_check(ctx, t, "3: ... that no unit takes, the full transport having no room for it",
              "local h = " + heads("full-t", "full-x") + " if h ~= '- -' then error(h) end " +
                  "if table.getn(__osc_rl['full-t']:GetCargo()) < 5 then error('unloaded') end");

    input.set_selected({id_of(ctx, "rclick-t")});
    const sim::Vector3 at = position_of(ctx, "rclick-x");
    const auto right = input.right_click_at(ctx.sim, at.x, at.z, false);
    t.check(right.size() == 1 && right.front().type == "TransportReverseLoadUnits",
            fmt::format("4: a right click on the tank with the transport selected is a reverse "
                        "load ({})",
                        right.empty() ? std::string("none") : right.front().type));
    tick_until(ctx, "4: the tank boards the transport", 600,
               "__osc_rl_aboard(__osc_rl['rclick-x'], __osc_rl['rclick-t'])");

    input.set_selected({id_of(ctx, "carrier")});
    issued = click_on("plane");
    t.check(issued && issued->type == "TransportReverseLoadUnits",
            fmt::format("5: a Transport click on an aircraft with a carrier selected is a reverse "
                        "load ({})",
                        issued ? issued->type : std::string("none")));
    const auto* carrier =
        static_cast<const sim::Unit*>(ctx.sim.entity_registry().find(id_of(ctx, "carrier")));
    const sim::EntityId plane = id_of(ctx, "plane");
    tick_until(ctx, "5: the aircraft lands on the carrier and is stored", 600,
               [&] { return carrier && carrier->is_stored_unit(plane); });

    input.set_selected({id_of(ctx, "pad")});
    issued = click_on("pad-plane");
    t.check(issued && issued->type == "TransportReverseLoadUnits",
            fmt::format("6: a Transport click on an aircraft with an air staging platform selected "
                        "is a reverse load ({})",
                        issued ? issued->type : std::string("none")));
    const auto* docked =
        static_cast<const sim::Unit*>(ctx.sim.entity_registry().find(id_of(ctx, "pad-plane")));
    const sim::EntityId pad = id_of(ctx, "pad");
    tick_until(ctx, "6: the aircraft docks at the platform", 600,
               [&] { return docked && docked->transport_id() == pad; });
    tick_for(ctx, 1);
    lua_check(ctx, t, "6: docked, the aircraft's order is done and the platform keeps loading",
              "local h = " + heads("pad", "pad-plane") + " if h ~= '23 -' or " +
                  "not __osc_rl['pad']:IsUnitState('TransportLoading') then error(h) end");

    t.check(osc::test_status::failure_count() - t.fail == failures_before, "7: no script errors");
    spdlog::info("Reverse load test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
