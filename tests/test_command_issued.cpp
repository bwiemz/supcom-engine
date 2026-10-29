#include <catch2/catch_test_macros.hpp>

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

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct IssueWorld {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
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
