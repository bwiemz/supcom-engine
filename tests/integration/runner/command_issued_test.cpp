#include "command_issued_test.hpp"

#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "vfs/virtual_file_system.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <string>

namespace osc::test {

namespace {

std::string ui_answer(TestContext& ui, const std::string& body) {
    auto r =
        ui.lua_state.do_string("rawset(_G, '__osc_ci_answer', (function() " + body + " end)())");
    if (!r) {
        return "script error: " + r.error().message;
    }
    lua_State* L = ui.L;
    lua_pushstring(L, "__osc_ci_answer");
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string answer = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return answer;
}

constexpr const char* kWatch = R"(
    local commandmode = import('/lua/ui/game/commandmode.lua')
    local told = commandmode.OnCommandIssued
    __osc_ci_seen = {}
    commandmode.OnCommandIssued = function(command)
        table.insert(__osc_ci_seen, command)
        return told(command)
    end
    return 'ok'
)";

constexpr const char* kMove = R"(
    import('/lua/ui/game/commandmode.lua').StartCommandMode('order', {name = 'RULEUCC_Move'})
    return 'ok'
)";

constexpr const char* kAttackMove = R"(
    import('/lua/ui/game/commandmode.lua').StartCommandMode('order', {
        name = 'RULEUCC_Script', AbilityName = 'AttackMove', TaskName = 'AttackMove',
        Cursor = 'ATTACK_MOVE' })
    return 'ok'
)";

constexpr const char* kSelect = R"(
    SelectUnits(GetArmyAvatars())
    return GetSelectedUnits() and GetSelectedUnits()[1] and 'ok' or 'not selectable yet'
)";

constexpr const char* kEnhance = R"(
    -- FAF's construction panel names them when it shows the enhancement tab
    local bp = GetSelectedUnits()[1]:GetBlueprint()
    bp.Enhancements.AdvancedEngineering.ID = 'AdvancedEngineering'
    bp.Enhancements.AdvancedEngineering.UnitID = bp.BlueprintId
    __osc_ci_seen = {}
    IssueCommand('UNITCOMMAND_Script', {TaskName = 'EnhanceTask', Enhancement = 'AdvancedEngineering'},
                 true)
    local c = __osc_ci_seen[1]
    if table.getn(__osc_ci_seen) ~= 1 then return 'told ' .. table.getn(__osc_ci_seen) end
    if c.CommandType ~= 'Script' or not c.Clear then return 'told ' .. tostring(c.CommandType) end
    if not (c.LuaParams and c.LuaParams.TaskName == 'EnhanceTask'
            and c.LuaParams.Enhancement == 'AdvancedEngineering') then
        return 'LuaParams ' .. tostring(c.LuaParams and c.LuaParams.Enhancement)
    end
    return 'ok'
)";

constexpr const char* kQueued = R"(
    local acu = GetSelectedUnits()[1]
    local queue = import('/lua/ui/notify/enhancementqueue.lua').getEnhancementQueue()
    return tostring(table.getn(queue[acu:GetEntityId()] or {}))
)";

constexpr const char* kStop = R"(
    __osc_ci_seen = {}
    IssueUnitCommand(GetSelectedUnits(), 'Stop')
    local c = __osc_ci_seen[1]
    if not c or c.CommandType ~= 'Stop' or c.LuaParams ~= nil then return 'told ' .. tostring(c) end
    return 'ok'
)";

constexpr const char* kFactories = R"(
    local fs = EntityCategoryFilterDown(categories.ueb0101, GetIdleFactories() or {})
    local es = EntityCategoryFilterDown(categories.uel0105, GetIdleEngineers() or {})
    if not (fs[1] and fs[2] and es[1]) then return 'factories ' .. table.getn(fs) end
    __osc_ci_units = {F1 = fs[1], F2 = fs[2], E = es[1]}
    __osc_ci_queue = function(name)
        local text = ''
        for _, b in SetCurrentFactoryForQueueDisplay(__osc_ci_units[name]) or {} do
            text = text .. b.id .. 'x' .. b.count .. ' '
        end
        ClearCurrentFactoryForQueueDisplay()
        return text
    end
    return 'ok'
)";

const sim::Unit* commander(TestContext& ui) {
    const auto* e = ui.sim.entity_registry().find(army_acu_id(ui.sim, 0));
    return e && e->is_unit() ? static_cast<const sim::Unit*>(e) : nullptr;
}

std::string queue_text(const sim::Unit& u) {
    std::string text;
    for (const auto& c : u.command_queue()) {
        text += fmt::format("{}({:.0f},{:.0f}) ", static_cast<int>(c.type), c.target_pos.x,
                            c.target_pos.z);
    }
    return text;
}

bool ends_at(const sim::Unit& u, sim::CommandType type, f32 x, f32 z) {
    if (u.command_queue().empty()) {
        return false;
    }
    const auto& last = u.command_queue().back();
    return last.type == type && std::abs(last.target_pos.x - x) < 0.5f &&
           std::abs(last.target_pos.z - z) < 0.5f;
}

} // namespace

void test_command_issued(TestContext& ui, const std::function<void(int)>& pump,
                         const std::function<void(int)>& play,
                         const std::function<bool(f32, f32, bool)>& click,
                         const std::function<bool(const char*)>& sim_lua) {
    spdlog::info("=== Command issued: the UI's orders reach OnCommandIssued ===");
    Tally t;
    const bool faf = ui.vfs.file_exists("/lua/ui/notify/enhancementqueue.lua");
    spdlog::info("command-issued-test: {}'s scripts", faf ? "FAF" : "retail");
    t.check(ui_answer(ui, kWatch) == "ok", "Test 1: OnCommandIssued watched");
    std::string selected;
    const sim::Unit* acu = nullptr;
    for (int i = 0; i < 100 && !(selected == "ok" && acu && !acu->block_command_queue()); ++i) {
        play(1);
        selected = ui_answer(ui, kSelect);
        acu = commander(ui);
    }
    const bool ready = acu && selected == "ok" && !acu->block_command_queue();
    t.check(ready, "Test 2: the commander takes orders: " + selected);
    if (!ready) {
        return;
    }

    const std::string enhanced = ui_answer(ui, kEnhance);
    t.check(enhanced == "ok", "Test 3: the enhancement's order tells OnCommandIssued, its table "
                              "as LuaParams: " +
                                  enhanced);
    play(1);
    t.check(!acu->command_queue().empty() &&
                acu->command_queue().front().type == sim::CommandType::Script,
            "Test 4: the commander takes the enhancement: " + queue_text(*acu));
    if (faf) {
        const std::string queued = ui_answer(ui, kQueued);
        t.check(queued == "1", "Test 5: FAF's enhancement queue holds it: " + queued);
    }
    const std::string stopped = ui_answer(ui, kStop);
    t.check(stopped == "ok", "Test 6: Stop tells OnCommandIssued: " + stopped);
    play(1);
    t.check(acu->command_queue().empty(), "Test 7: the commander stops: " + queue_text(*acu));
    if (faf) {
        const std::string queued = ui_answer(ui, kQueued);
        t.check(queued == "0", "Test 8: FAF's Stop empties its enhancement queue: " + queued);
    }

    sim_lua(R"(
        local p = ArmyBrains[1]:GetListOfUnits(categories.COMMAND, false)[1]:GetPosition()
        for _, d in {{-12, -12}, {12, -12}} do
            local x, z = p[1] + d[1], p[3] + d[2]
            CreateUnitHPR('ueb0101', 'ARMY_1', x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        end
        local x, z = p[1] + 6, p[3] + 6
        CreateUnitHPR('uel0105', 'ARMY_1', x, GetSurfaceHeight(x, z), z, 0, 0, 0)
    )");
    play(2);
    const std::string factories = ui_answer(ui, kFactories);
    t.check(factories == "ok", "Test 11: two factories and an engineer: " + factories);
    struct Build {
        const char* selection;
        const char* args;
        const char* queues;
    };
    const Build builds[] = {
        {"u.F1", "'UNITCOMMAND_BuildFactory', 'uel0105', 3", "uel0105x3 |"},
        {"u.F1, u.F2, u.E", "'UNITCOMMAND_BuildFactory', 'uel0105', 2", "uel0105x2 |uel0105x2 "},
        {"u.F1", "'UNITCOMMAND_BuildFactory', 'uel0105', 3, true", "uel0105x1 |"},
    };
    for (const Build& b : builds) {
        if (factories != "ok") {
            break;
        }
        const std::string issued =
            ui_answer(ui, fmt::format("local u = __osc_ci_units SelectUnits({{{}}}) "
                                      "IssueBlueprintCommand({}) return 'ok'",
                                      b.selection, b.args));
        play(2);
        const std::string queues =
            ui_answer(ui, "return __osc_ci_queue('F1') .. '|' .. __osc_ci_queue('F2')");
        t.check(issued == "ok" && queues == b.queues,
                fmt::format("Test 12: IssueBlueprintCommand({}) to {} queues {} (want {})", b.args,
                            b.selection, queues, b.queues));
        ui_answer(ui, "local u = __osc_ci_units IssueUnitCommand({u.F1, u.F2, u.E}, 'Stop') "
                      "return 'ok'");
        play(2);
    }
    ui_answer(ui, kSelect);
    if (!faf) {
        pump(1);
        return;
    }

    const f32 x = acu->position().x;
    const f32 z = acu->position().z;
    const auto attack_move = [&](f32 at_x, f32 at_z, bool shift) {
        const bool issued = ui_answer(ui, kAttackMove) == "ok" && click(at_x, at_z, shift);
        play(2);
        return issued;
    };
    const bool moved = ui_answer(ui, kMove) == "ok" && click(x + 20.0f, z + 20.0f, false);
    play(1);
    const bool shifted = attack_move(x - 20.0f, z + 20.0f, true);
    t.check(moved && shifted && !acu->command_queue().empty() &&
                acu->command_queue().front().type == sim::CommandType::Move &&
                ends_at(*acu, sim::CommandType::AggressiveMove, x - 20.0f, z + 20.0f),
            "Test 9: a Shift attack-move keeps the queued move and ends in an AggressiveMove: " +
                queue_text(*acu));
    const bool replaced = attack_move(x + 20.0f, z - 20.0f, false);
    t.check(replaced && acu->command_queue().size() == 1 &&
                ends_at(*acu, sim::CommandType::AggressiveMove, x + 20.0f, z - 20.0f),
            "Test 10: an attack-move replaces the queue with an AggressiveMove: " +
                queue_text(*acu));
    sim_lua("IssueClearCommands(ArmyBrains[1]:GetListOfUnits(categories.COMMAND, false))");
    play(1);
    pump(1);
}

} // namespace osc::test
