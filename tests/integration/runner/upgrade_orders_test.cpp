// --upgrade-orders-test: the upgrade orders a unit takes, as Moho's
// ProcessIssuedUnitCommand takes them (faf-re sim/Sim.cpp).

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <string>
#include <vector>

namespace osc::test {

namespace {

std::string lua_string(lua::LuaState& state, const std::string& expr) {
    auto r = state.do_string("return tostring(" + expr + ")");
    if (!r) {
        return "error: " + r.error().message;
    }
    lua_State* L = state.raw();
    std::string value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return value;
}

sim::Unit* unit_of(TestContext& ctx, const std::string& expr) {
    if (!ctx.lua_state.do_string("return " + expr + ":GetEntityId()")) {
        return nullptr;
    }
    auto* e =
        ctx.sim.entity_registry().find(sim::to_entity_id(ctx.L, -1, ctx.sim.entity_registry()));
    lua_pop(ctx.L, 1);
    return e && e->is_unit() ? static_cast<sim::Unit*>(e) : nullptr;
}

} // namespace

void test_upgrade_orders(TestContext& ctx) {
    spdlog::info("=== UPGRADE ORDERS TEST: the upgrades a unit takes ===");
    Tally t;
    const bool faf = lua_string(ctx.lua_state, "rawget(_G, 'MergeArmyPair') ~= nil") == "true";
    for (const char* bp : {"ueb0101", "ueb0201", "ueb0301", "ueb1103", "ueb1202", "ueb1302"}) {
        spdlog::info(
            "upgrade-orders-test: {} UpgradesFrom {} UpgradesFromBase {} UpgradesTo {}", bp,
            lua_string(ctx.lua_state, fmt::format("__blueprints.{}.General.UpgradesFrom", bp)),
            lua_string(ctx.lua_state, fmt::format("__blueprints.{}.General.UpgradesFromBase", bp)),
            lua_string(ctx.lua_state, fmt::format("__blueprints.{}.General.UpgradesTo", bp)));
    }

    struct Case {
        const char* name;
        const char* unit;
        const char* orders;
        const char* queue;
        const char* faf_queue;
    };
    const Case cases[] = {
        {"twice", "ueb0101", "'ueb0201', 'ueb0201'", "1 1 ", nullptr},
        {"t1-to-t3", "ueb0101", "'ueb0301'", "1 ", nullptr},
        {"foreign", "ueb0101", "'uel0105'", "0 ", nullptr},
        {"engineer", "uel0105", "'ueb0201'", "0 ", nullptr},
        {"t2-then-t3", "ueb0101", "'ueb0201', 'ueb0301'", "1 2 ", nullptr},
        {"t3-then-t2", "ueb0101", "'ueb0301', 'ueb0201'", "1 2 ", nullptr},
        {"t2-to-t2", "ueb0201", "'ueb0201'", "0 ", "1 "},
        {"t2-to-t3", "ueb0201", "'ueb0301'", "1 ", nullptr},
        {"t2-to-t1", "ueb0201", "'ueb0101'", "0 ", "1 "},
        {"mex-t1-to-t3", "ueb1103", "'ueb1302'", "1 ", nullptr},
        {"mex-twice", "ueb1103", "'ueb1202', 'ueb1202'", "1 1 ", nullptr},
        {"twice-build-between", "ueb0101", "'ueb0201', 'BUILD', 'ueb0201'", "1 2 3 ", nullptr},
        {"acu", "uel0001", "'ueb0201'", "0 ", nullptr},
    };
    if (auto r = ctx.lua_state.do_string(R"(
        local brain = GetArmyBrain('ARMY_1')
        local x, z = brain:GetArmyStartPos()
        __uo_n = 0
        function __uo_queue_after(bp, orders)
            local i = __uo_n
            __uo_n = __uo_n + 1
            local ux, uz = x - 40 + 16 * math.mod(i, 6), z + 30 + 16 * math.floor(i / 6)
            local u = CreateUnitHPR(bp, 'ARMY_1', ux, GetTerrainHeight(ux, uz), uz, 0, 0, 0)
            local trail = ''
            for _, to in orders do
                if to == 'BUILD' then
                    IssueBuildFactory({u}, 'uel0105', 1)
                else
                    IssueUpgrade({u}, to)
                end
                trail = trail .. table.getn(u:GetCommandQueue()) .. ' '
            end
            return trail
        end
    )");
        !r) {
        test_status::fail("[FAIL] upgrade-orders-test setup: {}", r.error().message);
        return;
    }
    for (const Case& c : cases) {
        const std::string queue = lua_string(
            ctx.lua_state, fmt::format("__uo_queue_after('{}', {{{}}})", c.unit, c.orders));
        const char* expected = faf && c.faf_queue ? c.faf_queue : c.queue;
        t.check(queue == expected,
                fmt::format("a script's IssueUpgrade, {} ({} to {}): queue after "
                            "each order {}(expected {})",
                            c.name, c.unit, c.orders, queue, expected));
    }

    if (auto r = ctx.lua_state.do_string(R"(
        local brain = GetArmyBrain('ARMY_1')
        local x, z = brain:GetArmyStartPos()
        __uo_a = CreateUnitHPR('ueb0101', 'ARMY_1', x - 20, GetTerrainHeight(x - 20, z - 30),
                               z - 30, 0, 0, 0)
        __uo_b = CreateUnitHPR('ueb0101', 'ARMY_1', x + 20, GetTerrainHeight(x + 20, z - 30),
                               z - 30, 0, 0, 0)
        __uo_c = CreateUnitHPR('ueb0101', 'ARMY_1', x, GetTerrainHeight(x, z - 50), z - 50, 0, 0,
                               0)
    )");
        !r) {
        test_status::fail("[FAIL] upgrade-orders-test factories: {}", r.error().message);
        return;
    }
    sim::Unit* a = unit_of(ctx, "__uo_a");
    sim::Unit* b = unit_of(ctx, "__uo_b");
    sim::Unit* c = unit_of(ctx, "__uo_c");
    if (!a || !b || !c) {
        test_status::fail("[FAIL] upgrade-orders-test: no factories");
        return;
    }
    const auto click = [&](sim::Unit& u, bool clear, bool build = false) {
        sim::UnitCommand cmd;
        cmd.type = build ? sim::CommandType::BuildFactory : sim::CommandType::Upgrade;
        cmd.blueprint_id = build ? "uel0105" : "ueb0201";
        ctx.sim.set_human_input_active(true);
        ctx.sim.route_player_command({u.entity_id()}, cmd, clear);
        ctx.sim.set_human_input_active(false);
    };
    const auto shown = [&](const sim::Unit& u) {
        const auto pending = ctx.sim.queues_with_pending();
        const auto it = pending.find(u.entity_id());
        return it == pending.end() ? u.command_queue().size() : it->second.orders.size();
    };
    click(*a, false);
    click(*a, false);
    click(*b, true);
    click(*b, true);
    click(*c, false, true);
    click(*c, false);
    click(*c, true);
    t.check(shown(*a) == 1 && shown(*b) == 1,
            fmt::format("two upgrade clicks, before the beat: queues shown {} and, cleared, {}",
                        shown(*a), shown(*b)));
    tick_until(ctx, "upgrade-orders-test: the clicks applied", 10,
               [&] { return !a->command_queue().empty() && !b->command_queue().empty(); });
    tick_for(ctx, 5);
    t.check(a->command_queue().size() == 1 && b->command_queue().size() == 1,
            fmt::format("two upgrade clicks: queues {} and, cleared, {}", a->command_queue().size(),
                        b->command_queue().size()));
    t.check(c->command_queue().size() == 2,
            fmt::format("a build, an upgrade, and the upgrade again cleared: queue {}",
                        c->command_queue().size()));
    click(*a, false);
    tick_for(ctx, 5);
    t.check(
        a->command_queue().size() == 1,
        fmt::format("a third click, the upgrade under way: queue {}", a->command_queue().size()));
    spdlog::info("upgrade-orders-test: {} passed, {} failed ({} data)", t.pass, t.fail,
                 faf ? "FAF's" : "retail's");
}

} // namespace osc::test
