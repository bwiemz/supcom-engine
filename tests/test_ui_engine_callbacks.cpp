#include <catch2/catch_test_macros.hpp>

#include "core/game_state.hpp"
#include "lua/lua_state.hpp"

#include <string>

namespace {

struct UiState {
    osc::lua::LuaState lua;

    UiState() {
        REQUIRE(lua.do_string("heard = {} "
                              "modules = {} "
                              "function import(name) "
                              "  local m = modules[name] "
                              "  if not m then error('no module ' .. name) end "
                              "  return m "
                              "end")
                    .ok());
    }
    void run(const std::string& code) { REQUIRE(lua.do_string(code).ok()); }
    bool check(const std::string& expr) { return lua.do_string("assert(" + expr + ")").ok(); }
};

} // namespace

TEST_CASE("Closing the window asks uimain's ShowEscapeDialog", "[ui][lua]") {
    UiState ui;
    CHECK_FALSE(osc::core::call_show_escape_dialog(ui.lua.raw()));
    ui.run("modules['/lua/ui/uimain.lua'] = {ShowEscapeDialog = function(yesNoOnly) "
           "  table.insert(heard, tostring(yesNoOnly)) end}");
    CHECK(osc::core::call_show_escape_dialog(ui.lua.raw()));
    CHECK(ui.check("table.getn(heard) == 1 and heard[1] == 'true'"));
}

TEST_CASE("The world camera's tracking reaches tracking.lua's OnTrackUnit", "[ui][lua]") {
    UiState ui;
    ui.run("modules['/lua/ui/game/tracking.lua'] = {OnTrackUnit = function(camera, tracking) "
           "  table.insert(heard, camera .. ':' .. tostring(tracking)) end}");
    osc::core::call_on_track_unit(ui.lua.raw(), true);
    osc::core::call_on_track_unit(ui.lua.raw(), false);
    CHECK(ui.check("heard[1] == 'WorldCamera:true' and heard[2] == 'WorldCamera:false'"));
}

TEST_CASE("The command graph shown reaches commandgraph.lua's OnCommandGraphShow", "[ui][lua]") {
    UiState ui;
    ui.run("modules['/lua/ui/game/commandgraph.lua'] = {OnCommandGraphShow = function(show) "
           "  table.insert(heard, tostring(show)) end}");
    osc::core::call_on_command_graph_show(ui.lua.raw(), true);
    osc::core::call_on_command_graph_show(ui.lua.raw(), false);
    CHECK(ui.check("heard[1] == 'true' and heard[2] == 'false'"));
}
