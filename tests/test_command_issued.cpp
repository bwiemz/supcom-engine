#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/input_handler.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/manipulator.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "ui/ui_control.hpp"

#include <lua.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct IssueWorld {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::blueprints::BlueprintStore store{ui.raw()};
    osc::ui::UIControlRegistry registry;
    osc::renderer::InputHandler input;
    osc::u32 id = 0;

    IssueWorld() {
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_blueprint_id("uel0001");
        unit->add_category("SELECTABLE");
        id = sim.entity_registry().register_entity(std::move(unit));
        osc::lua::register_moho_bindings(ui, sim);
        osc::lua::register_ui_bindings(ui, registry);
        osc::lua::register_user_bindings(ui);
        lua_State* L = ui.raw();
        ui.set_blueprint_store(&store);
        for (const char* bp : {"uel0101", "uel0105", "ueb0101", "ueb0201"}) {
            REQUIRE(ui.do_string(std::string("return {BlueprintId = '") + bp + "'}").ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        lua_pushstring(L, "__osc_input_handler");
        lua_pushlightuserdata(L, &input);
        lua_rawset(L, LUA_REGISTRYINDEX);
        input.set_selected({id});
        osc::sim::push_entity_id(L, id);
        lua_setglobal(L, "id");
        osc::lua::push_units_for_ui(L, {id});
        lua_setglobal(L, "units");
        REQUIRE(ui.do_string(R"(
            seen = {}
            function OnCommandIssued(command) table.insert(seen, command) end
        )")
                    .ok());
    }

    /// A unit of army 1 of `bp` and `category`, called `name` in what the
    /// UI is told (told()).
    osc::u32 add_unit(const char* bp, const char* category, const char* name) {
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_blueprint_id(bp);
        unit->add_category("SELECTABLE");
        unit->add_category(category);
        const osc::u32 added = sim.entity_registry().register_entity(std::move(unit));
        lua_State* L = ui.raw();
        lua_getglobal(L, "names");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setglobal(L, "names");
        }
        osc::sim::push_entity_id(L, added);
        lua_pushstring(L, name);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        names[added] = name;
        return added;
    }

    /// What OnCommandIssued was told since the last call: per order, its
    /// type, blueprint, units and a '!' if it clears.
    std::string told() {
        REQUIRE(ui.do_string(R"(
            local text = ''
            for _, c in seen do
                local units = ''
                for _, u in c.Units do units = units .. (names[u:GetEntityId()] or '?') end
                text = text .. c.CommandType .. ':' .. c.Blueprint .. ':' .. units
                    .. (c.Clear and '!' or '') .. ' '
            end
            seen = {}
            told_text = text
        )")
                    .ok());
        lua_State* L = ui.raw();
        lua_getglobal(L, "told_text");
        std::string text = lua_tostring(L, -1);
        lua_pop(L, 1);
        return text;
    }

    /// The orders scheduled since the last call, as told() writes them.
    std::string scheduled() {
        const auto pending = sim.command_scheduler().pending();
        std::string text;
        for (std::size_t i = scheduled_; i < pending.size(); ++i) {
            const auto& c = pending[i];
            text += c.command.blueprint_id + ':';
            for (const osc::u32 unit : c.unit_ids) {
                text += names[unit];
            }
            text += c.clear_existing ? "! " : " ";
        }
        scheduled_ = pending.size();
        return text;
    }

    /// The error `code` raises, or "" when it ran.
    std::string error_of(const std::string& code) {
        auto r = ui.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }

    std::map<osc::u32, std::string> names;
    std::size_t scheduled_ = 0;

    bool check(const std::string& code) {
        auto r = ui.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }
};

} // namespace

TEST_CASE("A UI script's order tells OnCommandIssued, its Lua data as LuaParams",
          "[ui][command_issued]") {
    IssueWorld w;
    REQUIRE(w.check("data = { TaskName = 'EnhanceTask', Enhancement = 'AdvancedEngineering' }"
                    " IssueCommand('UNITCOMMAND_Script', data, false)"));
    CHECK(w.check("assert(table.getn(seen) == 1, 'told ' .. table.getn(seen))"
                  " local c = seen[1]"
                  " assert(c.CommandType == 'Script' and c.Clear == false, 'kind')"
                  " assert(c.LuaParams ~= data and c.LuaParams.TaskName == 'EnhanceTask'"
                  "   and c.LuaParams.Enhancement == 'AdvancedEngineering', 'params')"
                  " assert(c.Blueprint == '' and c.Target.Type == 'None', 'target')"
                  " local p = c.Target.Position"
                  " assert(p[1] == 0 and p[2] == 0 and p[3] == 0, 'position')"
                  " assert(table.getn(c.Units) == 1 and c.Units[1]:GetEntityId() == id, 'units')"));
}

TEST_CASE("Each order a UI script issues through Moho's ISSUE_Command tells OnCommandIssued",
          "[ui][command_issued]") {
    IssueWorld w;
    static_cast<osc::sim::Unit*>(w.sim.entity_registry().find(w.id))->add_category("FACTORY");
    REQUIRE(w.check("IssueCommand('UNITCOMMAND_Stop')"
                    " IssueUnitCommand(units, 'UNITCOMMAND_Dive')"
                    " IssueUnitCommand(units, 'UNITCOMMAND_Stop', 'text')"
                    " IssueBlueprintCommand('UNITCOMMAND_Upgrade', 'ueb0201', 1, false)"
                    " IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0101', 2)"
                    " IssueCommand('UNITCOMMAND_BuildSiloNuke')"));
    CHECK(w.check("local told = ''"
                  " for _, c in seen do"
                  "   told = told .. c.CommandType .. (c.Clear and '!' or '') .. c.Blueprint"
                  "     .. (c.LuaParams and '=' .. c.LuaParams or '') .. ' '"
                  " end"
                  " assert(told == 'Stop! Dive! Stop!=text Upgradeueb0201 BuildFactoryuel0101"
                  " BuildFactoryuel0101 ', told)"));
}

TEST_CASE("A command-mode Script order hands OnCommandIssued VerifyScriptCommand's answer",
          "[ui][command_issued][script]") {
    IssueWorld w;
    lua_State* L = w.ui.raw();
    REQUIRE(w.check("answer = { TaskName = 'AttackMove', UserValidated = true }"));
    lua_getglobal(L, "answer");
    const auto args = osc::sim::lua_to_bytes(L, -1);
    lua_pop(L, 1);
    REQUIRE(args);
    osc::renderer::CommandMode mode;
    mode.mode = "order";
    mode.name = "RULEUCC_Script";
    mode.verify_script = [&](const std::vector<osc::u32>&, osc::u32, const osc::sim::Vector3&,
                             bool) {
        return std::optional(osc::renderer::VerifiedScript{{w.id}, *args});
    };
    const auto issued = w.input.click_in_command_mode(w.sim, mode, 40.0f, 60.0f, true);
    REQUIRE(issued);
    osc::lua::report_command_issued(L, *issued);
    CHECK(w.check("local c = seen[1]"
                  " assert(c.CommandType == 'Script' and c.Clear == false, 'kind')"
                  " assert(c.LuaParams.TaskName == 'AttackMove' and c.LuaParams.UserValidated,"
                  "   'params')"
                  " assert(c.Target.Type == 'Position' and c.Target.Position[1] == 40"
                  "   and c.Target.Position[3] == 60, 'target')"));
}

TEST_CASE("IssueBlueprintCommand's factory build goes count times to each selected factory, each "
          "order told",
          "[ui][command_issued]") {
    IssueWorld w;
    const osc::u32 f1 = w.add_unit("ueb0101", "FACTORY", "F1");
    const osc::u32 f2 = w.add_unit("ueb0101", "FACTORY", "F2");
    const osc::u32 e = w.add_unit("uel0105", "ENGINEER", "E");
    struct Case {
        std::unordered_set<osc::u32> selection;
        const char* args;
        const char* told;
        const char* scheduled;
    };
    const Case cases[] = {
        {{f1},
         "'uel0105', 3",
         "BuildFactory:uel0105:F1 BuildFactory:uel0105:F1 BuildFactory:uel0105:F1 ",
         "uel0105:F1 uel0105:F1 uel0105:F1 "},
        {{f1, f2, e},
         "'uel0105', 2",
         "BuildFactory:uel0105:F1 BuildFactory:uel0105:F1 BuildFactory:uel0105:F2 "
         "BuildFactory:uel0105:F2 ",
         "uel0105:F1 uel0105:F1 uel0105:F2 uel0105:F2 "},
        {{e}, "'uel0105', 1", "", ""},
        {{f1},
         "'uel0105', 3, true",
         "BuildFactory:uel0105:F1! BuildFactory:uel0105:F1! BuildFactory:uel0105:F1! ",
         "uel0105:F1! uel0105:F1! uel0105:F1! "},
        {{f1}, "'UEL0105', 1", "BuildFactory:uel0105:F1 ", "uel0105:F1 "},
        {{f1},
         "'uel0105', 2.7",
         "BuildFactory:uel0105:F1 BuildFactory:uel0105:F1 ",
         "uel0105:F1 uel0105:F1 "},
        {{f1}, "'uel0105', 0", "", ""},
        {{f1}, "'uel0105', -1", "", ""},
        {{f1}, "'xxx9999', 1", "", ""},
    };
    for (const Case& c : cases) {
        INFO(c.args);
        w.input.set_selected(c.selection);
        REQUIRE(w.check(std::string("IssueBlueprintCommand('UNITCOMMAND_BuildFactory', ") + c.args +
                        ")"));
        CHECK(w.told() == c.told);
        CHECK(w.scheduled() == c.scheduled);
    }
}

TEST_CASE("IssueBlueprintCommand gives other orders count times to the whole selection, as Moho's",
          "[ui][command_issued]") {
    IssueWorld w;
    const osc::u32 f1 = w.add_unit("ueb0101", "FACTORY", "F1");
    const osc::u32 e = w.add_unit("uel0105", "ENGINEER", "E");
    w.input.set_selected({f1, e});
    REQUIRE(w.check("IssueBlueprintCommand('UNITCOMMAND_Upgrade', 'ueb0201', 2, false)"));
    CHECK(w.told() == "Upgrade:ueb0201:F1E Upgrade:ueb0201:F1E ");
    CHECK(w.scheduled() == "ueb0201:F1E ueb0201:F1E ");
    REQUIRE(w.check("IssueBlueprintCommand('UNITCOMMAND_Stop', 'uel0105', 2)"));
    CHECK(w.told() == "Stop:uel0105:F1E Stop:uel0105:F1E ");
    REQUIRE(w.check("IssueBlueprintCommand('UNITCOMMAND_BuildSiloNuke', 'uel0105', 3)"));
    CHECK(w.told().empty());
}

TEST_CASE("The UI's issue functions take and refuse their arguments in Moho's words",
          "[ui][command_issued]") {
    IssueWorld w;
    const osc::u32 f1 = w.add_unit("ueb0101", "FACTORY", "F1");
    w.input.set_selected({f1});
    osc::lua::push_units_for_ui(w.ui.raw(), {f1});
    lua_setglobal(w.ui.raw(), "units");
    const std::pair<const char*, const char*> refusals[] = {
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0105')",
         "IssueBlueprintCommand(command, blueprintid, count, clear = false)\n  expected between 3 "
         "and 4 args, but got 2"},
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0105', 'x')",
         "attempt to use as integer a string value"},
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0105', '2')",
         "attempt to use as integer a string value"},
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0105', nil)",
         "attempt to use as integer a nil value"},
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'xxx9999', 'x')",
         "attempt to use as integer a string value"},
        {"IssueBlueprintCommand(nil, 'uel0105', 1)", "attempt to use as string a nil value"},
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', {}, 1)",
         "attempt to use as string a table value"},
        {"IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0105', 1, 0)",
         "attempt to use as boolean a number value"},
        {"IssueCommand()",
         "IssueCommand(command,[string],[clear])\n  expected between 1 and 3 args, but got 0"},
        {"IssueCommand('UNITCOMMAND_Stop', nil, true, 1)",
         "IssueCommand(command,[string],[clear])\n  expected between 1 and 3 args, but got 4"},
        {"IssueCommand(nil)", "attempt to use as string a nil value"},
        {"IssueCommand('UNITCOMMAND_Stop', nil, 0)", "attempt to use as boolean a number value"},
        {"IssueUnitCommand()", "IssueUnitCommand(unitList,command,[string],[clear])\n  expected "
                               "between 1 and 4 args, but got 0"},
        {"IssueUnitCommand('x', 'UNITCOMMAND_Stop')", "Unit list expected as first argument"},
        {"IssueUnitCommand({units[1], 7}, 'UNITCOMMAND_Stop')",
         "Expected a game object. (Did you call with '.' instead of ':'?)"},
        {"IssueUnitCommand(units, nil)", "attempt to use as string a nil value"},
        {"IssueUnitCommand(units, 'UNITCOMMAND_Stop', nil, 0)",
         "attempt to use as boolean a number value"},
    };
    for (const auto& [call, error] : refusals) {
        INFO(call);
        CHECK(w.error_of(call).ends_with(error));
    }
    CHECK(w.told().empty());

    REQUIRE(w.check("IssueCommand('UNITCOMMAND_Stop', nil, nil)"
                    " IssueCommand('UNITCOMMAND_Stop', nil, false)"
                    " IssueUnitCommand(units, 'UNITCOMMAND_Stop', nil, false)"
                    " IssueUnitCommand(units, 'UNITCOMMAND_BuildSiloNuke')"
                    " IssueUnitCommand({}, 'UNITCOMMAND_Stop')"
                    " IssueBlueprintCommand('UNITCOMMAND_BuildFactory', 'uel0105', 1, nil)"));
    CHECK(w.told() == "Stop::F1 Stop::F1 Stop::F1 BuildSiloNuke::F1! Stop::! "
                      "BuildFactory:uel0105:F1 ");
}
