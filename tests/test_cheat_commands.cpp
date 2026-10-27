// The console's cheat commands, as Moho's (faf-re console/CConCommand.cpp):
// CreateUnit, PopupCreateUnitMenu, TeleportSelectedUnits and
// DestroySelectedUnits, which retail's debug key map binds with cheats on;
// and the saved lines behind ConExecuteSave and CON_ExecuteLastCommand.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/sim_sync.hpp"
#include "lua/user_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "ui/console.hpp"
#include "ui/ui_control.hpp"

#include <memory>
#include <string>
#include <variant>
#include <vector>

extern "C" {
#include <lua.h>
}

using Catch::Approx;
using osc::sim::Unit;

namespace {

/// A sim of two armies on flat ground at 10, and a UI state bound to it in a
/// session, with the console and a selection.
struct CheatWorld {
    osc::lua::LuaState sim_lua;
    osc::blueprints::BlueprintStore store{sim_lua.raw()};
    osc::sim::SimState sim{sim_lua.raw(), &store};
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::sim::SimCallbackQueue queue;
    osc::GameStateManager game;
    osc::ui::Console console;
    osc::renderer::InputHandler input;

    CheatWorld() {
        osc::sim::GameSetup setup;
        setup.scenario = "/maps/test/test_scenario.lua";
        osc::lua::register_moho_bindings(sim_lua, sim);
        osc::lua::register_sim_bindings(sim_lua, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(129) * 129, 1280);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_game_setup(setup);
        lua_State* L = sim_lua.raw();
        REQUIRE(sim_lua
                    .do_string("return {BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                               " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                               " Footprint = {SizeX = 1, SizeZ = 1},"
                               " Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 3}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(sim_lua
                    .do_string(R"(
                        Tank = setmetatable({}, {__index = moho.unit_methods})
                        Tank.__index = Tank
                    )")
                    .ok());
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        lua_pushstring(L, "tank");
        lua_getglobal(L, "Tank");
        lua_rawset(L, -3);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_settop(L, 0);

        osc::lua::register_moho_bindings(ui, sim);
        ui.set_blueprint_store(&store); // (as the game's UI has the blueprints)
        osc::lua::register_ui_bindings(ui, registry);
        osc::lua::register_user_bindings(ui);
        osc::lua::register_console_commands(console);
        osc::lua::register_session_console_commands(console);
        lua_State* U = ui.raw();
        for (const auto& [key, value] :
             std::vector<std::pair<const char*, void*>>{{"__osc_sim_callback_queue", &queue},
                                                        {"__osc_game_state_mgr", &game},
                                                        {"__osc_console", &console},
                                                        {"__osc_input_handler", &input}}) {
            lua_pushstring(U, key);
            lua_pushlightuserdata(U, value);
            lua_rawset(U, LUA_REGISTRYINDEX);
        }
        game.transition_to(osc::GameState::GAME, nullptr);
        osc::lua::set_focus_army(nullptr, U, 0);
    }

    /// A tank of `army` (1-based) at (x, z).
    Unit& make(int army, float x, float z) {
        REQUIRE(sim_lua
                    .do_string("made = CreateUnit('tank', " + std::to_string(army) + ", " +
                               std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        lua_getglobal(sim_lua.raw(), "made");
        lua_pushstring(sim_lua.raw(), "_c_object");
        lua_rawget(sim_lua.raw(), -2);
        auto* unit = static_cast<Unit*>(lua_touserdata(sim_lua.raw(), -1));
        lua_settop(sim_lua.raw(), 0);
        REQUIRE(unit);
        return *unit;
    }

    void run(const std::string& line) { console.execute(ui.raw(), line); }

    std::string ui_run(const std::string& code) {
        auto r = ui.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }

    /// The sim's ticks, the command stream's callbacks and orders applied.
    void ticks(int n) {
        for (const auto& cb : queue.drain()) sim.run_sim_callback(cb);
        for (int i = 0; i < n; ++i) sim.tick();
    }
};

} // namespace

TEST_CASE(
    "ConExecuteSave keeps the last 100 lines, the latest first; CON_ExecuteLastCommand reruns "
    "it",
    "[console]") {
    CheatWorld w;
    int ran = 0;
    w.console.add("Count", [&ran](lua_State*, const std::vector<std::string>&) { ++ran; });
    w.run("CON_ExecuteLastCommand"); // none saved: nothing
    CHECK(ran == 0);
    REQUIRE(w.ui_run("ConExecuteSave('Count') ConExecuteSave('')").empty());
    CHECK(ran == 1);
    REQUIRE(w.console.saved().size() == 1);
    w.run("CON_ExecuteLastCommand");
    CHECK(ran == 2);
    // ConExecute saves nothing
    REQUIRE(w.ui_run("ConExecute('Count')").empty());
    CHECK(w.console.saved().size() == 1);
    // A saved line that runs itself runs once more, not without end
    w.console.execute_save(w.ui.raw(), "Count; CON_ExecuteLastCommand");
    CHECK(ran == 5);
    w.run("CON_ExecuteLastCommand");
    CHECK(ran == 6);
    for (int i = 0; i < 120; ++i) w.console.execute_save(w.ui.raw(), "Count " + std::to_string(i));
    REQUIRE(w.console.saved().size() == 100);
    CHECK(w.console.saved().front() == "Count 119");
    CHECK(w.console.saved().back() == "Count 20");
}

TEST_CASE("CreateUnit makes a unit for the army given, or the focus army, where the cursor is",
          "[console][cheat]") {
    CheatWorld w;
    w.run("CreateUnit tank");
    w.run("CreateUnit TANK 1");
    w.run("CreateUnit tank 2");         // no such army
    w.run("CreateUnit no_such_unit 0"); // no such blueprint
    w.run("CreateUnit");                // no blueprint
    auto sent = w.queue.drain();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].func_name == osc::sim::kCreateUnitCallback);
    CHECK(std::get<std::string>(sent[0].args.at("Blueprint")) == "tank");
    CHECK(std::get<double>(sent[0].args.at("Army")) == 0.0);
    CHECK(std::get<double>(sent[1].args.at("Army")) == 1.0);
    CHECK(std::get<double>(sent[1].args.at("Heading")) == 0.0);

    // The sim makes it only with cheats on
    for (const auto& cb : sent) w.sim.run_sim_callback(cb);
    size_t units = 0;
    w.sim.entity_registry().for_each_unit([&](osc::sim::Entity&) { ++units; });
    CHECK(units == 0);
    w.sim.set_cheats_enabled(true);
    w.sim.run_sim_callback(sent[1]);
    w.sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
        ++units;
        CHECK(e.army() == 1);
    });
    CHECK(units == 1);
}

TEST_CASE("PopupCreateUnitMenu opens retail's create-unit dialog where the cursor is",
          "[console][cheat]") {
    CheatWorld w;
    REQUIRE(w.ui_run(R"(
        opened = {}
        modules = { ['/lua/ui/dialogs/createunit.lua'] = {
            CreateDialog = function(x, y) table.insert(opened, type(x) .. ' ' .. type(y)) end } }
        function import(name) return modules[name] end
    )")
                .empty());
    w.run("PopupCreateUnitMenu");
    CHECK(w.ui_run("assert(table.getn(opened) == 1 and opened[1] == 'number number')").empty());
}

TEST_CASE("TeleportSelectedUnits warps the focus army's selected units, a cheat the sim runs",
          "[console][cheat]") {
    CheatWorld w;
    Unit& mine = w.make(1, 20, 20);
    Unit& theirs = w.make(2, 30, 20);
    w.input.set_selected({mine.entity_id(), theirs.entity_id()});
    w.run("TeleportSelectedUnits");
    auto sent = w.queue.drain();
    REQUIRE(sent.size() == 1); // the focus army's only
    CHECK(sent[0].func_name == osc::sim::kWarpEntityCallback);
    CHECK(std::get<double>(sent[0].args.at("Entity")) == static_cast<double>(mine.entity_id()));
    // (no world view here: the cursor's place is the origin's corner)
    sent[0].args["X"] = 60.0;
    sent[0].args["Z"] = 70.0;
    w.sim.run_sim_callback(sent[0]);
    CHECK(mine.position().x == Approx(20.0f));
    w.sim.set_cheats_enabled(true);
    w.sim.run_sim_callback(sent[0]);
    CHECK(mine.pending_position().x == Approx(60.0f));
    CHECK(mine.pending_position().z == Approx(70.0f));
    CHECK(mine.pending_position().y == Approx(10.0f)); // on the ground there
}

TEST_CASE("DestroySelectedUnits gives the selection a DestroySelf order, refused without cheats",
          "[console][cheat]") {
    CheatWorld w;
    Unit& a = w.make(1, 20, 20);
    Unit& b = w.make(1, 30, 20);
    w.input.set_selected({a.entity_id()});
    w.run("DestroySelectedUnits");
    w.ticks(3);
    CHECK_FALSE(a.destroyed());
    w.sim.set_cheats_enabled(true);
    w.run("DestroySelectedUnits");
    w.ticks(3);
    CHECK(a.destroyed());
    CHECK_FALSE(b.destroyed());
    CHECK(osc::sim::moho_command_number(osc::sim::CommandType::DestroySelf, false) == 31);
}
