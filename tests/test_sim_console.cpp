// Moho's sim console (faf-re sim/CSimConCommand.cpp, sim/Sim.cpp,
// sim/SimDebugCommandRegistrations.cpp): DoSimCommand sends a line through the
// command stream with the cursor, the focus army and the selection; the sim
// parses it (Sim::TryParseSimCommand) and runs each command, one that needs
// cheats only with them on.

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
#include "sim/army_brain.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_console.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
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

/// A sim of two armies whose units' script class records what it is told,
/// and a UI state bound to it in a session, with the session's console.
struct ConWorld {
    osc::lua::LuaState sim_lua;
    osc::blueprints::BlueprintStore store{sim_lua.raw()};
    osc::sim::SimState sim{sim_lua.raw(), &store};
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::sim::SimCallbackQueue queue;
    osc::GameStateManager game;
    osc::ui::Console console;

    ConWorld() {
        osc::sim::GameSetup setup;
        setup.scenario = "/maps/test/test_scenario.lua";
        osc::lua::register_moho_bindings(sim_lua, sim);
        osc::lua::register_sim_bindings(sim_lua, sim);
        std::vector<osc::u16> heights(static_cast<size_t>(129) * 129, 1280);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_game_setup(setup);
        lua_State* L = sim_lua.raw();
        REQUIRE(sim_lua
                    .do_string("return {BlueprintId = 'tank', Categories = {'LAND', 'MOBILE'},"
                               " Defense = {MaxHealth = 100}, SizeX = 1, SizeZ = 1,"
                               " Footprint = {SizeX = 1, SizeZ = 1},"
                               " Physics = {MotionType = 'RULEUMT_Land'}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
        REQUIRE(sim_lua
                    .do_string(R"(
                        told = {}
                        Tank = setmetatable({}, {__index = moho.unit_methods})
                        Tank.__index = Tank
                        function Tank:OnKilled()
                            table.insert(told, 'killed ' .. moho.entity_methods.GetArmy(self))
                        end
                        function Tank:OnProductionActive() table.insert(told, 'producing') end
                        function Tank:OnProductionInActive() table.insert(told, 'idle') end
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
        osc::lua::register_ui_bindings(ui, registry);
        osc::lua::register_user_bindings(ui);
        osc::lua::register_session_console_commands(console);
        lua_State* U = ui.raw();
        lua_pushstring(U, "__osc_sim_callback_queue");
        lua_pushlightuserdata(U, &queue);
        lua_rawset(U, LUA_REGISTRYINDEX);
        game.transition_to(osc::GameState::GAME, nullptr);
        lua_pushstring(U, "__osc_game_state_mgr");
        lua_pushlightuserdata(U, &game);
        lua_rawset(U, LUA_REGISTRYINDEX);
    }

    /// A tank of `army` (1-based) at (x, z), as `name` in the sim's Lua.
    Unit& make(const char* name, int army, float x, float z) {
        REQUIRE(sim_lua
                    .do_string(std::string(name) + " = CreateUnit('tank', " + std::to_string(army) +
                               ", " + std::to_string(x) + ", 10, " + std::to_string(z) + ")")
                    .ok());
        lua_getglobal(sim_lua.raw(), name);
        lua_pushstring(sim_lua.raw(), "_c_object");
        lua_rawget(sim_lua.raw(), -2);
        auto* unit = static_cast<Unit*>(lua_touserdata(sim_lua.raw(), -1));
        lua_settop(sim_lua.raw(), 0);
        REQUIRE(unit);
        return *unit;
    }

    /// Run `code` in the sim, answering its error (empty if none).
    std::string run(const std::string& code) {
        auto r = sim_lua.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }

    /// The sim console's line, from the focus army `focus` with `selected`.
    void exec(const std::string& line, int focus = 0,
              const std::vector<osc::sim::EntityId>& selected = {}) {
        osc::sim::execute_debug_command(sim, sim_lua.raw(), line, {}, focus, selected);
    }

    std::string told() {
        REQUIRE(sim_lua.do_string("told_text = table.concat(told, ', ')").ok());
        lua_getglobal(sim_lua.raw(), "told_text");
        std::string out = lua_tostring(sim_lua.raw(), -1);
        lua_pop(sim_lua.raw(), 1);
        return out;
    }
};

} // namespace

TEST_CASE("The sim console's line parses as Moho's ParseOneSimCommand", "[sim-console]") {
    std::vector<std::string> tokens;
    std::string rest;
    osc::sim::parse_sim_command(R"(  Purge   unit 1; KillAll 0)", tokens, rest);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0] == "Purge");
    CHECK(tokens[2] == "1");
    CHECK(rest == " KillAll 0");
    // Quotes keep spaces, ';' and '#'; a backslash escapes inside them
    osc::sim::parse_sim_command(R"(SimLog "a; b # \"c\"" d # not this; nor this)", tokens, rest);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[1] == R"(a; b # "c")");
    CHECK(tokens[2] == "d");
    CHECK(rest.empty());
    // A long chain runs every command once
    {
        osc::sim::SimState sim(nullptr, nullptr);
        std::string line;
        for (int i = 0; i < 3000; ++i) line += "AI_RunOpponentAI;";
        osc::sim::execute_debug_command(sim, nullptr, line, {}, -1, {});
        CHECK(sim.con_vars().run_opponent_ai); // toggled an even number of times
        line += "AI_RunOpponentAI";
        osc::sim::execute_debug_command(sim, nullptr, line, {}, -1, {});
        CHECK_FALSE(sim.con_vars().run_opponent_ai);
    }
    // And back: a token with a space, ';' or '#' quoted
    CHECK(osc::sim::unparse_sim_command({"SimLog", "a b", "c;d", "e"}) ==
          R"(SimLog "a b" "c;d" e)");

    // Found in any case; AI_RunOpponentAI is the one that needs no cheats
    REQUIRE(osc::sim::find_sim_command("blingbling"));
    CHECK(std::string(osc::sim::find_sim_command("blingbling")->name) == "BlingBling");
    CHECK_FALSE(osc::sim::find_sim_command("NoSuchCommand"));
    CHECK(osc::sim::find_sim_command("ai_runopponentai")->allow_without_cheats);
    CHECK_FALSE(osc::sim::find_sim_command("NoDamage")->allow_without_cheats);
}

TEST_CASE("Without cheats a sim command that needs them ends the line; AI_RunOpponentAI runs",
          "[sim-console]") {
    ConWorld w;
    w.exec("NoDamage; AI_RunOpponentAI off");
    CHECK_FALSE(w.sim.con_vars().no_damage);
    CHECK(w.sim.con_vars().run_opponent_ai);
    w.exec("AI_RunOpponentAI off; NoDamage");
    CHECK_FALSE(w.sim.con_vars().run_opponent_ai);
    CHECK_FALSE(w.sim.con_vars().no_damage);
    // The brain's answer is /noai's and the variable's; the platoon's the variable's
    lua_State* L = w.sim_lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, w.sim.get_army(0));
    lua_rawset(L, -3);
    lua_setglobal(L, "brain");
    CHECK(w.run("assert(not moho.aibrain_methods.IsOpponentAIRunning(brain))").empty());
    CHECK(w.run("p = moho.aibrain_methods.MakePlatoon(brain, '', '')"
                " assert(not moho.platoon_methods.IsOpponentAIRunning(p))")
              .empty());
    w.exec("AI_RunOpponentAI on");
    CHECK(w.run("assert(moho.aibrain_methods.IsOpponentAIRunning(brain))").empty());
    w.sim.set_run_opponent_ai(false); // /noai
    CHECK(w.run("assert(not moho.aibrain_methods.IsOpponentAIRunning(brain))").empty());
    CHECK(w.run("assert(moho.platoon_methods.IsOpponentAIRunning(p))").empty());
}

TEST_CASE("NoDamage: AdjustHealth takes nothing away; a dead unit is healed by nothing either",
          "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    Unit& t = w.make("t1", 1, 20, 20);
    // Its bool toggles bare, and reads on/off/tog
    w.exec("NoDamage");
    CHECK(w.sim.con_vars().no_damage);
    CHECK(w.run("moho.entity_methods.AdjustHealth(t1, nil, -30)"
                " moho.entity_methods.SetHealth(t1, nil, 10)")
              .empty());
    CHECK(t.health() == Approx(100.0f));
    w.exec("nodamage off");
    CHECK_FALSE(w.sim.con_vars().no_damage);
    CHECK(w.run("moho.entity_methods.AdjustHealth(t1, nil, -30)").empty());
    CHECK(t.health() == Approx(70.0f));
    w.exec("NoDamage tog");
    CHECK(w.sim.con_vars().no_damage);
    w.exec("NoDamage = 0");
    CHECK_FALSE(w.sim.con_vars().no_damage);

    t.begin_dying();
    CHECK(w.run("moho.entity_methods.AdjustHealth(t1, nil, 20)").empty());
    CHECK(t.health() == Approx(70.0f));
}

TEST_CASE("BlingBling fills the focus army's storage 10000 deeper; ZeroExtraStorage takes it away",
          "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    osc::sim::ArmyBrain* brain = w.sim.get_army(1);
    REQUIRE(brain);
    const float mass_max = brain->economy().mass.max_storage;
    const float mass_stored = brain->economy().mass.stored;
    w.exec("BlingBling", 1);
    CHECK(brain->extra_storage_mass() == Approx(10000.0));
    CHECK(brain->extra_storage_energy() == Approx(10000.0));
    CHECK(brain->economy().mass.max_storage == Approx(mass_max + 10000.0f));
    CHECK(brain->economy().mass.stored == Approx(mass_stored + mass_max + 10000.0f));
    // Not army 0's
    CHECK(w.sim.get_army(0)->extra_storage_mass() == Approx(0.0));
    // No focus army: nothing
    w.exec("BlingBling", -1);
    CHECK(brain->extra_storage_mass() == Approx(10000.0));
    w.exec("ZeroExtraStorage", 1);
    CHECK(brain->extra_storage_mass() == Approx(0.0));
    CHECK(brain->economy().mass.max_storage == Approx(mass_max));
}

TEST_CASE("SallyShears turns the fog of war over", "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    w.sim.set_fog_of_war("explored");
    w.exec("SallyShears");
    CHECK(w.sim.fog_mode() == osc::sim::FogMode::None);
    w.exec("SallyShears");
    CHECK(w.sim.fog_mode() == osc::sim::FogMode::Explored);
}

TEST_CASE("KillAll and DestroyAll take the armies named, 0-based, or every army", "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    Unit& a = w.make("a", 1, 20, 20);
    Unit& b = w.make("b", 2, 30, 20);
    // An army that isn't there, or isn't a number: no unit at all
    w.exec("KillAll 7 x");
    CHECK_FALSE(a.is_dying());
    CHECK_FALSE(b.is_dying());
    w.exec("KillAll 1");
    CHECK_FALSE(a.is_dying());
    CHECK(b.is_dying());
    CHECK(w.told() == "killed 2");
    w.exec("DestroyAll");
    CHECK(a.destroyed());
    CHECK(b.destroyed());
}

TEST_CASE("Purge destroys every entity of a kind, of an army if named", "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    Unit& a = w.make("a", 1, 20, 20);
    Unit& b = w.make("b", 2, 30, 20);
    w.exec("Purge nothing");
    w.exec("Purge units 1");
    CHECK_FALSE(a.destroyed());
    CHECK(b.destroyed());
    w.exec("Purge unit");
    CHECK(a.destroyed());
}

TEST_CASE("DamageUnit deals the selected units Debug damage; the toggles switch their economy",
          "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    Unit& a = w.make("a", 1, 20, 20);
    Unit& b = w.make("b", 1, 30, 20);
    w.exec("DamageUnit 25", 0, {a.entity_id()});
    CHECK(a.health() == Approx(75.0f));
    CHECK(b.health() == Approx(100.0f));
    // None more for nothing after armour (SIM_Damage)
    w.exec("DamageUnit -25", 0, {a.entity_id()});
    CHECK(a.health() == Approx(75.0f));
    // The selection is a set: a unit named twice is dealt it once
    w.exec("DamageUnit 5", 0, {a.entity_id(), a.entity_id()});
    CHECK(a.health() == Approx(70.0f));

    w.exec("DebugSetProductionInActive", 0, {b.entity_id(), a.entity_id()});
    CHECK_FALSE(a.economy().production_active);
    w.exec("DebugSetProductionActive", 0, {b.entity_id()});
    CHECK(b.economy().production_active);
    CHECK(w.told() == "idle, idle, producing");
    w.exec("DebugSetConsumptionActive", 0, {a.entity_id()});
    CHECK(a.economy().consumption_active);
    w.exec("DebugSetConsumptionInActive", 0, {a.entity_id()});
    CHECK_FALSE(a.economy().consumption_active);
}

TEST_CASE("ScenarioMethod calls the scenario's own; never one of Lua's loaders", "[sim-console]") {
    ConWorld w;
    w.sim.set_cheats_enabled(true);
    REQUIRE(w.run("called = 0 ScenarioInfo = {Env = setmetatable({OnF3 = function()"
                  "   called = called + 1 end}, {__index = _G})}"
                  " load_it = dofile")
                .empty());
    w.exec("ScenarioMethod OnF3; ScenarioMethod NoSuch");
    CHECK(w.run("assert(called == 1)").empty());
    // dofile() would read standard input; by name or another, it isn't run
    w.exec("ScenarioMethod dofile; ScenarioMethod load_it");
    CHECK(w.run("assert(called == 1)").empty());
}

TEST_CASE("DoSimCommand sends the line through the command stream; each sim command's alias too",
          "[sim-console]") {
    ConWorld w;
    lua_State* U = w.ui.raw();
    // Without a session nothing goes
    w.game.transition_to(osc::GameState::FRONT_END, nullptr);
    w.console.execute(U, "DoSimCommand NoDamage");
    CHECK(w.queue.empty());
    w.game.transition_to(osc::GameState::GAME, nullptr);
    w.console.execute(U, "DoSimCommand");
    CHECK(w.queue.empty());

    osc::lua::set_focus_army(nullptr, U, 1);
    w.console.execute(U, R"(DoSimCommand SimLog "a b" c)");
    w.console.execute(U, "blingbling");
    auto sent = w.queue.drain();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].func_name == osc::sim::kDebugCommandCallback);
    CHECK(std::get<std::string>(sent[0].args.at("Command")) == R"(SimLog "a b" c)");
    CHECK(std::get<double>(sent[0].args.at("FocusArmy")) == 1.0);
    // The alias puts the command's own name first
    CHECK(std::get<std::string>(sent[1].args.at("Command")) == "BlingBling");

    // The sim runs it with cheats on, for the focus army it names
    w.sim.set_cheats_enabled(true);
    for (const auto& cb : sent) w.sim.run_sim_callback(cb);
    CHECK(w.sim.get_army(1)->extra_storage_mass() == Approx(10000.0));
    CHECK(w.sim.get_army(0)->extra_storage_mass() == Approx(0.0));
}
