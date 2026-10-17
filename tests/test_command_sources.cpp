// Command sources (Moho's per-army mValidCommandSources): which players an
// army takes orders and callbacks from, what a script sees of the running
// source (GetCurrentCommandSource, OkayToMessWithArmy), and FAF's
// SetCommandSource, by which its union and common armies share control.

#include <catch2/catch_test_macros.hpp>

#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/session_manager.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/sim_sync.hpp"
#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "ui/ui_control.hpp"
#include "vfs/virtual_file_system.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::sim::BrainState;
using osc::sim::CommandType;
using osc::sim::SimCallbackEntry;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

/// Three armies (ARMY_1..3, 0-based 0..2), the sim's and the UI's bindings
/// in one state, and a stand-in /lua/SimCallbacks.lua whose DoCallback
/// notes what a script sees of the running source in the global `seen`.
struct SourcesWorld {
    osc::lua::LuaState lua;
    SimState sim{lua.raw(), nullptr};

    SourcesWorld() {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_sim_bindings(lua, sim);
        sim.set_victory_condition("sandbox");
        for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3"}) sim.add_army(army, army);
        REQUIRE(run(R"(
            local module = {
                DoCallback = function(name, args, units)
                    seen = {
                        source = GetCurrentCommandSource(),
                        okay = { OkayToMessWithArmy(1), OkayToMessWithArmy(2),
                                 OkayToMessWithArmy(3) },
                    }
                end,
            }
            import = function(path) return module end
        )")
                    .empty());
    }

    /// Run `code`; the error message, or "" when it ran.
    std::string run(const std::string& code) {
        auto r = lua.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }

    /// The value of the Lua expression `expr`, as a string ("nil", "true",
    /// "2", ...).
    std::string value_of(const std::string& expr) {
        auto r = lua.do_string("return tostring(" + expr + ")");
        REQUIRE(r.ok());
        lua_State* L = lua.raw();
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }

    osc::u32 spawn(osc::i32 army) {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
        u->set_max_speed(6.0f); // so a Move lasts past the tick it starts in
        return sim.entity_registry().register_entity(std::move(u));
    }
    Unit& unit(osc::u32 id) { return *static_cast<Unit*>(sim.entity_registry().find(id)); }
    size_t orders(osc::u32 id) { return unit(id).command_queue().size(); }

    /// `source` orders `ids` to move, appending to their queues.
    void order(osc::u32 source, const std::vector<osc::u32>& ids) {
        UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = {100.0f, 0.0f, 100.0f};
        sim.schedule_command(source, ids, move, false);
        sim.tick();
    }

    /// `source` runs a (script) callback.
    void callback(osc::u32 source) {
        SimCallbackEntry cb;
        cb.func_name = "Probe";
        sim.schedule_callback(source, cb);
        sim.tick();
    }

    /// A game's numbering: player 0 plays army 0, the AI army 1, and player
    /// 1 army 2.
    void number_players() {
        sim.begin_command_sources();
        sim.set_command_source(0, 0, true);
        sim.set_command_source(2, 1, true);
    }
};

/// Take the pending focus-army request from `L`'s registry ("nil" if none).
std::string take_focus_request(lua_State* L) {
    lua_pushstring(L, osc::lua::kFocusArmyRequestKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    std::string v = lua_isnumber(L, -1) ? std::to_string(static_cast<int>(lua_tonumber(L, -1)))
                                        : std::string("nil");
    lua_pop(L, 1);
    lua_pushstring(L, osc::lua::kFocusArmyRequestKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return v;
}

} // namespace

TEST_CASE("Before a game numbers its players, any source commands any army", "[sources]") {
    SourcesWorld w;
    const auto ai = w.spawn(1);
    w.order(5, {ai});
    CHECK(w.orders(ai) == 1);
    w.callback(5);
    CHECK(w.value_of("seen.source") == "6");
    CHECK(w.value_of("seen.okay[2]") == "true");
}

TEST_CASE("A player's orders move only the armies it commands", "[sources]") {
    SourcesWorld w;
    w.number_players();
    const auto mine = w.spawn(0);
    const auto ai = w.spawn(1);
    const auto theirs = w.spawn(2);
    w.sim.set_recording(true);

    w.order(0, {mine, ai, theirs});
    CHECK(w.orders(mine) == 1);
    CHECK(w.orders(ai) == 0);
    CHECK(w.orders(theirs) == 0);
    // The recording keeps the order as applied
    REQUIRE(w.sim.recorded_replay().commands.size() == 1);
    CHECK(w.sim.recorded_replay().commands[0].unit_ids == std::vector<osc::u32>{mine});

    w.order(1, {mine, theirs});
    CHECK(w.orders(mine) == 1);
    CHECK(w.orders(theirs) == 1);
    // A source no army lists (an observer's) moves nothing
    w.order(2, {mine, ai, theirs});
    CHECK(w.orders(mine) == 1);
    CHECK(w.orders(ai) == 0);
    CHECK(w.orders(theirs) == 1);
}

TEST_CASE("A player's callbacks reach only the units it commands", "[sources]") {
    SourcesWorld w;
    w.number_players();
    const auto mine = w.spawn(0);
    const auto theirs = w.spawn(2);
    SimCallbackEntry hold;
    hold.func_name = osc::sim::kUnitSettingCallback;
    hold.args["Setting"] = std::string("FireState");
    hold.args["Value"] = 1.0;
    hold.unit_ids = {mine, theirs};
    w.sim.schedule_callback(1, hold);
    w.sim.tick();
    CHECK(w.unit(mine).fire_state() == 0);
    CHECK(w.unit(theirs).fire_state() == 1);
}

TEST_CASE("A script sees the source running a callback", "[sources]") {
    SourcesWorld w;
    w.number_players();
    w.callback(1);
    CHECK(w.value_of("seen.source") == "2"); // 1-based
    CHECK(w.value_of("seen.okay[1]") == "false");
    CHECK(w.value_of("seen.okay[2]") == "false"); // the AI's: no player's
    CHECK(w.value_of("seen.okay[3]") == "true");
    // Outside a source's command, none runs
    CHECK(w.value_of("GetCurrentCommandSource()") == "nil");
    CHECK(w.value_of("OkayToMessWithArmy(1)") == "false");
    // An army that doesn't exist, no one may
    w.callback(0);
    CHECK(w.value_of("seen.okay[1]") == "true");
    CHECK(w.value_of("OkayToMessWithArmy(9)") == "false");
}

TEST_CASE("Union army: SetCommandSource shares an ally's control, and takes it back",
          "[sources][faf]") {
    // FAF's BeginSessionUnionArmy: SetCommandSource(ally - 1, humanIndex, true)
    SourcesWorld w;
    w.number_players();
    const auto ai = w.spawn(1);
    REQUIRE(w.run("SetCommandSource(1, 0, true)").empty()); // 0-based both
    CHECK(w.sim.army_command_sources(1) == std::vector<osc::u32>{0});
    w.order(0, {ai});
    CHECK(w.orders(ai) == 1);
    w.callback(0);
    CHECK(w.value_of("seen.okay[2]") == "true");

    REQUIRE(w.run("SetCommandSource(1, 0, false)").empty());
    w.order(0, {ai});
    CHECK(w.orders(ai) == 1);
    // No such army, or a source that is no number: nothing
    REQUIRE(w.run("SetCommandSource(7, 0, true) SetCommandSource(-1, 0, true)"
                  "SetCommandSource(0, 0/0, true)")
                .empty());
    CHECK(w.sim.army_command_sources(0) == std::vector<osc::u32>{0});
}

TEST_CASE("Common army: MergeArmyPair moves a player's control and focus to the team's army",
          "[sources][faf]") {
    // FAF's simInit.MergeArmyPair(into = 3, from = 1, comm, humanIndex = 0)
    SourcesWorld w;
    w.number_players();
    const auto mine = w.spawn(0);
    const auto team = w.spawn(2);
    REQUIRE(w.run(R"(
        SetCommandSource(1 - 1, 0, false)
        SetCommandSource(3 - 1, 0, true)
        SetFocusArmy(3 - 1)
    )")
                .empty());
    // FAF's sim SetFocusArmy is 0-based, and waits for the next beat
    CHECK(take_focus_request(w.lua.raw()) == "2");
    w.order(0, {mine, team});
    CHECK(w.orders(mine) == 0);
    CHECK(w.orders(team) == 1);
    // Both players command the team's army now
    CHECK(w.sim.army_command_sources(2) == std::vector<osc::u32>{0, 1});
    CHECK(w.sim.army_command_sources(0).empty());
    // An observer's focus, and no army past the last
    REQUIRE(w.run("SetFocusArmy(-1)").empty());
    CHECK(take_focus_request(w.lua.raw()) == "-1");
    REQUIRE(w.run("SetFocusArmy(3)").empty());
    CHECK(take_focus_request(w.lua.raw()) == "nil");
}

TEST_CASE("An army out of the game takes no one's orders; cheats take any", "[sources]") {
    SourcesWorld w;
    w.number_players();
    const auto mine = w.spawn(0);
    const auto ai = w.spawn(1);
    w.sim.get_army(0)->set_state(BrainState::Defeat);
    w.order(0, {mine});
    CHECK(w.orders(mine) == 0);
    w.callback(0);
    CHECK(w.value_of("seen.okay[1]") == "false");
    CHECK(w.value_of("CheatsEnabled()") == "false");

    w.sim.set_cheats_enabled(true);
    w.order(0, {ai});
    CHECK(w.orders(ai) == 1);
    w.callback(2); // even an observer
    CHECK(w.value_of("seen.okay[2]") == "true");
    CHECK(w.value_of("seen.okay[1]") == "false"); // but out of the game is out
    CHECK(w.value_of("CheatsEnabled()") == "true");
}

TEST_CASE("An army a dropped player leaves with no player is abandoned to its brain",
          "[sources][drop]") {
    SourcesWorld w;
    w.number_players();
    // Army 2 (player 1's) is shared with player 0, as a union army does
    w.sim.set_command_source(2, 0, true);
    // Armies 0 and 2 have scripts: AbandonedByPlayer notes the army
    lua_State* L = w.lua.raw();
    for (const osc::i32 army : {0, 2}) {
        REQUIRE(w.run("return { AbandonedByPlayer = function(self) abandoned = (abandoned or '') .."
                      " self.name end, name = 'ARMY_" +
                      std::to_string(army + 1) + "' }")
                    .empty());
        w.sim.get_army(army)->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }
    const auto terminate = [&](osc::u32 source) {
        SimCallbackEntry cb;
        cb.func_name = osc::sim::kCommandSourceTerminatedCallback;
        cb.args["Source"] = static_cast<double>(source);
        w.sim.schedule_callback(osc::sim::kEngineSource, cb);
        w.sim.tick();
    };
    // Player 1 goes: army 2 keeps player 0
    terminate(1);
    CHECK(w.value_of("abandoned") == "nil");
    CHECK(w.sim.army_command_sources(2) == std::vector<osc::u32>{0});
    // Player 0 goes: both its armies are abandoned, in order
    terminate(0);
    CHECK(w.value_of("abandoned") == "ARMY_1ARMY_3");
    CHECK_FALSE(w.sim.army_at(0)->is_defeated()); // the brains decide
    CHECK_FALSE(w.sim.army_at(1)->is_defeated()); // no player's: never abandoned

    // A brain without the callback is defeated outright
    SourcesWorld bare;
    bare.number_players();
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kCommandSourceTerminatedCallback;
    cb.args["Source"] = 1.0;
    bare.sim.schedule_callback(osc::sim::kEngineSource, cb);
    bare.sim.tick();
    CHECK(bare.sim.army_at(2)->is_defeated());
    CHECK_FALSE(bare.sim.army_at(0)->is_defeated());
}

TEST_CASE("A player's frame can't end a source or defeat an army", "[sources][drop]") {
    SourcesWorld w;
    w.number_players();
    SimCallbackEntry end;
    end.func_name = osc::sim::kCommandSourceTerminatedCallback;
    end.args["Source"] = 0.0;
    SimCallbackEntry defeat;
    defeat.func_name = osc::sim::kDefeatArmyCallback;
    defeat.args["Army"] = 0.0;
    w.sim.schedule_callback(1, end);
    w.sim.schedule_callback(1, defeat);
    w.sim.tick();
    CHECK(w.sim.army_command_sources(0) == std::vector<osc::u32>{0});
    CHECK_FALSE(w.sim.army_at(0)->is_defeated());
}

TEST_CASE("A lobby's game gives each army its owner's source, as the lobby numbered them",
          "[sources][lobby]") {
    struct MpReset {
        MpReset() { osc::lua::mp_net_state().reset(); }
        ~MpReset() { osc::lua::mp_net_state().reset(); }
    } reset;
    const auto start = [](bool lobby) {
        osc::lua::LuaState lua;
        SimState sim(lua.raw(), nullptr);
        osc::lua::SessionManager mgr;
        osc::lua::ScenarioMetadata meta;
        meta.armies = {"ARMY_1", "ARMY_2", "ARMY_3", "ARMY_4"};
        for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3", "ARMY_4"}) sim.add_army(army, army);
        mgr.set_ai_armies({1});
        REQUIRE(lua.do_string("ScenarioInfo = {}").ok());
        auto& mp = osc::lua::mp_net_state();
        mp.reset();
        if (lobby) {
            // Source 0 owns slots 1 and 4, source 1 slot 3; slot 2 an AI's
            mp.transport_ready = true;
            mp.army_sources = {0, -1, 1, 0};
        }
        REQUIRE(mgr.start_session(lua, osc::vfs::VirtualFileSystem{}, sim, meta).ok());
        mp.reset();
        std::vector<std::vector<osc::u32>> out;
        out.reserve(4);
        for (osc::i32 army = 0; army < 4; ++army) out.push_back(sim.army_command_sources(army));
        return out;
    };
    using Sources = std::vector<std::vector<osc::u32>>;
    // The lobby's, whatever the players' order would give
    CHECK(start(true) == Sources{{0}, {}, {1}, {0}});
    // Without one, the players' armies in order
    CHECK(start(false) == Sources{{0}, {}, {1}, {2}});
}

TEST_CASE("Command sources and the cheats option survive a save", "[sources][save]") {
    SourcesWorld w;
    w.number_players();
    w.sim.set_command_source(1, 0, true);
    w.sim.set_cheats_enabled(true);
    const auto bytes = osc::sim::save_sim_state(w.sim);

    osc::lua::LuaState lua;
    SimState loaded(lua.raw(), nullptr); // booted for the same game
    loaded.set_victory_condition("sandbox");
    for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3"}) loaded.add_army(army, army);
    const std::string error = osc::sim::load_sim_state(loaded, bytes);
    INFO(error);
    REQUIRE(error.empty());
    CHECK(loaded.command_sources_known());
    CHECK(loaded.army_command_sources(0) == std::vector<osc::u32>{0});
    CHECK(loaded.army_command_sources(1) == std::vector<osc::u32>{0});
    CHECK(loaded.army_command_sources(2) == std::vector<osc::u32>{1});
    CHECK(loaded.cheats_option());
}

TEST_CASE("A player may watch only an army it commands, as Moho's session allows",
          "[sources][focus]") {
    // CWldSession::ValidateFocusArmyRequest, in single-player (source 0)
    SourcesWorld w;
    w.number_players();
    // The UI's state, in a world session
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(ui, w.sim);
    osc::lua::register_ui_bindings(ui, registry);
    lua_State* L = ui.raw();
    lua_pushstring(L, osc::core::kWorldUiActiveKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    const auto run = [&](const char* code) {
        auto r = ui.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    };

    REQUIRE(run("SetFocusArmy(1)").empty()); // 1-based: its own
    CHECK(take_focus_request(L) == "0");
    REQUIRE(run("SetFocusArmy(3)").empty());
    CHECK(take_focus_request(L) == "nil");
    REQUIRE(run("SetFocusArmy(-1)").empty()); // no observing allowed
    CHECK(take_focus_request(L) == "nil");
    CHECK_FALSE(run("SetFocusArmy(4)").empty()); // no such army: an error

    // Shared with it (FAF's union army), the army may be watched
    w.sim.set_command_source(2, 0, true);
    REQUIRE(run("SetFocusArmy(3)").empty());
    CHECK(take_focus_request(L) == "2");

    // Observing allowed: once out of the game, any army
    osc::sim::GameSetup setup;
    setup.options.configured = true;
    setup.options.set_bool("AllowObservers", true);
    w.sim.set_game_setup(setup);
    REQUIRE(run("SetFocusArmy(2)").empty());
    CHECK(take_focus_request(L) == "nil"); // still playing
    w.sim.get_army(0)->set_state(BrainState::Defeat);
    w.sim.get_army(2)->set_state(BrainState::Defeat);
    REQUIRE(run("SetFocusArmy(2)").empty());
    CHECK(take_focus_request(L) == "1");

    // Cheats, or the game over: any
    w.sim.get_army(0)->set_state(BrainState::InProgress);
    REQUIRE(run("SetFocusArmy(2)").empty());
    CHECK(take_focus_request(L) == "nil");
    w.sim.set_game_ended(true);
    REQUIRE(run("SetFocusArmy(2)").empty());
    CHECK(take_focus_request(L) == "1");
}
