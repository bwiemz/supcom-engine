// Retail's army statistics window (/lua/debug/ArmyStats.lua, the
// ShowArmyStats console command): the UI asks the sim through ExecLuaInSim
// (Moho's cfunc_ExecLuaInSimL, a cheat the sim runs as Sim::ExecuteLuaInSim)
// to SetArmyStatsSyncArmy, and each beat's Sync then carries that army's
// stats tree as __ArmyStats (Sim::Sync), the tick on it.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/sim_sync.hpp"
#include "lua/user_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "ui/console.hpp"
#include "ui/ui_control.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

extern "C" {
#include <lua.h>
}

namespace {

/// Run `code` in `lua`, answering its error (empty if none)
std::string run(osc::lua::LuaState& lua, const char* code) {
    auto r = lua.do_string(code);
    return r.ok() ? std::string() : r.error().message;
}

/// A sim, its state's bindings, and a UI state bound to it in a session
struct StatsWorld {
    osc::lua::LuaState sim_lua;
    osc::blueprints::BlueprintStore store{sim_lua.raw()};
    osc::sim::SimState sim{sim_lua.raw(), &store};
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::sim::SimCallbackQueue queue;
    osc::GameStateManager game;

    StatsWorld() {
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        osc::lua::register_moho_bindings(sim_lua, sim);
        osc::lua::register_sim_bindings(sim_lua, sim);
        osc::lua::register_moho_bindings(ui, sim);
        osc::lua::register_ui_bindings(ui, registry);
        osc::lua::register_user_bindings(ui);
        lua_State* L = ui.raw();
        lua_pushstring(L, "__osc_sim_callback_queue");
        lua_pushlightuserdata(L, &queue);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    void start_session() {
        game.transition_to(osc::GameState::GAME, nullptr);
        lua_State* L = ui.raw();
        lua_pushstring(L, "__osc_game_state_mgr");
        lua_pushlightuserdata(L, &game);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
};

} // namespace

TEST_CASE("ExecLuaInSim sends its call through the command stream; the sim runs it with cheats "
          "on",
          "[stats][cheat]") {
    StatsWorld w;
    // Without a session nothing goes
    REQUIRE(run(w.ui, "ExecLuaInSim('Probe', {x = 5})").empty());
    CHECK(w.queue.empty());
    w.start_session();
    CHECK(run(w.ui, "ExecLuaInSim('Probe')").find("expected 2 args, but got 1") !=
          std::string::npos);
    CHECK(run(w.ui, "ExecLuaInSim({}, 1)").find("string expected") != std::string::npos);
    CHECK(w.queue.empty());

    REQUIRE(run(w.ui, "ExecLuaInSim('Probe', {x = 5})").empty());
    REQUIRE(run(w.ui, "ExecLuaInSim('Broken', 1)").empty());
    REQUIRE(run(w.ui, "ExecLuaInSim('NoSuchFunction', nil)").empty());
    const auto sent = w.queue.drain();
    REQUIRE(sent.size() == 3);
    CHECK(sent[0].func_name == osc::sim::kExecLuaInSimCallback);
    CHECK(std::get<std::string>(sent[0].args.at("Func")) == "Probe");
    REQUIRE(sent[0].lua_args);

    REQUIRE(run(w.sim_lua, "probed = nil "
                           "function Probe(a) probed = a.x end "
                           "function Broken(a) error('broken') end")
                .empty());
    // Cheats off: nothing runs
    w.sim.run_sim_callback(sent[0]);
    CHECK(run(w.sim_lua, "assert(probed == nil)").empty());
    // On: it runs; an error, or no such function, is logged
    w.sim.set_cheats_enabled(true);
    for (const auto& cb : sent) w.sim.run_sim_callback(cb);
    CHECK(run(w.sim_lua, "assert(probed == 5)").empty());

    // Never Lua's loaders, by name or another: dofile would run a file from
    // this machine's disk for another player's command
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "osc_exec_lua_in_sim_probe.lua";
    {
        std::ofstream out(file);
        out << "loaded = true\n";
    }
    REQUIRE(run(w.sim_lua, "loaded = nil load_it = dofile").empty());
    for (const char* loader : {"dofile", "load_it"}) {
        osc::sim::SimCallbackEntry cb = sent[0];
        cb.args["Func"] = std::string(loader);
        lua_pushstring(w.sim_lua.raw(), file.string().c_str());
        cb.lua_args = osc::sim::lua_to_bytes(w.sim_lua.raw(), -1);
        lua_pop(w.sim_lua.raw(), 1);
        w.sim.run_sim_callback(cb);
    }
    CHECK(run(w.sim_lua, "assert(loaded == nil)").empty());
    // (The file itself loads)
    CHECK(run(w.sim_lua, ("load_it('" + file.generic_string() + "') assert(loaded)").c_str())
              .empty());
    std::filesystem::remove(file);

    // A script's SimCallback may not send the engine's own; one of its own goes
    REQUIRE(run(w.ui, "SimCallback({Func = '__osc_ExecLuaInSim', Args = {}})").empty());
    CHECK(w.queue.empty());
    REQUIRE(run(w.ui, "SimCallback({Func = 'Probe', Args = {x = 1}})").empty());
    CHECK(w.queue.drain().size() == 1);
}

TEST_CASE("SetArmyStatsSyncArmy puts the army's stats in each beat's Sync", "[stats]") {
    StatsWorld w;
    lua_State* sim_L = w.sim_lua.raw();
    CHECK(w.sim.sync_army() == -1);
    CHECK(run(w.sim_lua, "SetArmyStatsSyncArmy()").find("expected 1 args, but got 0") !=
          std::string::npos);
    CHECK(run(w.sim_lua, "SetArmyStatsSyncArmy('1')").find("integer expected") !=
          std::string::npos);
    REQUIRE(run(w.sim_lua, "SetArmyStatsSyncArmy(1)").empty());
    CHECK(w.sim.sync_army() == 1);

    osc::sim::ArmyBrain* brain = w.sim.get_army(1);
    REQUIRE(brain);
    brain->set_stat("Units_Active", 3);
    brain->set_stat("Economy_TotalProduced_Mass", 120.5);
    brain->record_enemy_killed("uel0101", 52, 260, false);
    w.sim.get_army(0)->set_stat("Units_Active", 9);
    REQUIRE(run(w.sim_lua, "Sync = {} function ResetSyncTable() Sync = {} end").empty());

    osc::lua::SyncQueue sync;
    sync.queue(sim_L, nullptr);
    sync.deliver(sim_L, w.ui.raw());
    const std::string error = run(w.ui, R"(
        local stats = Sync.__ArmyStats
        assert(stats, 'no stats')
        assert(stats.Name == 'Root' and stats.Tick == 0, 'root')
        local by = {}
        for _, c in stats.Children do by[c.Name] = c end
        -- In the names' order: Economy, Enemies, Units
        assert(stats.Children[1].Name == 'Economy' and stats.Children[3].Name == 'Units', 'order')
        local mass = by.Economy.Children[1].Children[1]
        assert(mass.Name == 'Mass' and mass.Type == 'Float' and mass.Value == 120.5, 'mass')
        assert(by.Units.Children[1].Name == 'Active' and by.Units.Children[1].Value == 3, 'army 2')
        local killed
        for _, c in by.Enemies.Children do if c.Name == 'Killed' then killed = c end end
        assert(killed.Value == 1 and killed.Blueprints.uel0101 == '1.00', 'blueprints')
    )");
    INFO(error);
    CHECK(error.empty());

    // None: the next beat's Sync has none
    REQUIRE(run(w.sim_lua, "SetArmyStatsSyncArmy(-1)").empty());
    sync.queue(sim_L, nullptr);
    sync.deliver(sim_L, w.ui.raw());
    CHECK(run(w.ui, "assert(Sync.__ArmyStats == nil)").empty());
}

TEST_CASE("ShowArmyStats shows the focus army's stats, or the army asked for", "[stats][console]") {
    StatsWorld w;
    osc::ui::Console console;
    osc::lua::register_session_console_commands(console);
    REQUIRE(run(w.ui, R"(
        shown = {}
        modules = { ['/lua/debug/ArmyStats.lua'] = {
            Show = function(army, section) table.insert(shown, army .. ' ' .. section) end } }
        function import(name) return modules[name] end
    )")
                .empty());
    console.execute(w.ui.raw(), "ShowArmyStats");
    console.execute(w.ui.raw(), "ShowArmyStats 1 units");
    CHECK(run(w.ui, "assert(table.getn(shown) == 2 and shown[1] == '0 all' and "
                    "shown[2] == '1 units', tostring(shown[1]))")
              .empty());

    // An observer has none
    osc::lua::set_focus_army(nullptr, w.ui.raw(), -1);
    console.execute(w.ui.raw(), "ShowArmyStats");
    CHECK(run(w.ui, "assert(table.getn(shown) == 2)").empty());
}
