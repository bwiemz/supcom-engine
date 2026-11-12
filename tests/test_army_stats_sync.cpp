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
#include "sim/manipulator.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "ui/console.hpp"
#include "ui/ui_control.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

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

namespace {

/// Whether `xml` is well formed, as far as a GameStats document goes: its
/// declaration, then only whitespace between elements, each attribute value
/// quoted with no '<' or '"' in it and no '&' but the five named entities,
/// and every element closed in order.
bool well_formed(const std::string& xml) {
    const std::string decl = "<?xml version=\"1.0\" ?>\n";
    if (xml.compare(0, decl.size(), decl) != 0) return false;
    const auto entity_at = [&](size_t at) {
        for (const char* e : {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"}) {
            if (xml.compare(at, std::strlen(e), e) == 0) return true;
        }
        return false;
    };
    const auto name_end = [&](size_t at) {
        while (at < xml.size() &&
               (std::isalnum(static_cast<unsigned char>(xml[at])) || xml[at] == ':')) {
            ++at;
        }
        return at;
    };
    std::vector<std::string> open;
    size_t i = decl.size();
    while (i < xml.size()) {
        if (xml[i] == ' ' || xml[i] == '\n') {
            ++i;
            continue;
        }
        if (xml[i] != '<') return false;
        const bool closing = xml.compare(i, 2, "</") == 0;
        const size_t start = i + (closing ? 2 : 1);
        size_t j = name_end(start);
        const std::string name = xml.substr(start, j - start);
        if (name.empty()) return false;
        if (closing) {
            if (j >= xml.size() || xml[j] != '>' || open.empty() || open.back() != name) {
                return false;
            }
            open.pop_back();
            i = j + 1;
            continue;
        }
        for (;;) {
            while (j < xml.size() && xml[j] == ' ') ++j;
            if (j >= xml.size()) return false;
            if (xml[j] == '>') {
                open.push_back(name);
                ++j;
                break;
            }
            if (xml.compare(j, 2, "/>") == 0) {
                j += 2;
                break;
            }
            const size_t attribute = j;
            j = name_end(j);
            if (j == attribute || xml.compare(j, 2, "=\"") != 0) return false;
            for (j += 2; j < xml.size() && xml[j] != '"'; ++j) {
                if (xml[j] == '<' || (xml[j] == '&' && !entity_at(j))) return false;
            }
            if (j >= xml.size()) return false;
            ++j;
        }
        i = j;
    }
    return open.empty();
}

} // namespace

// SubmitXMLArmyStats (retail's victory.lua, at a defeat and at the game's end)
// asks the beat's sync for Moho's <GameStats> document (Sim::Sync,
// CArmyStats::ArmyXmlStatsNode), which the user side hands GPGNet.
const char* const kGameStats = R"xml(<?xml version="1.0" ?>
<GameStats xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">
  <Army index="0" name="Tester">
      <UnitStats>
        <Unit id="bomber" type="Bomber" built="0" lost="0" killed="1" damagedealt="0.00" damagereceived="0.00" masscost="210.33" energycost="7000.00" buildtime="1600.00"/>
        <Unit id="pgen" type="Power Generator" built="1" lost="0" killed="0" damagedealt="0.00" damagereceived="0.00" masscost="75.00" energycost="750.00" buildtime="125.50"/>
        <Unit id="tank" type="&lt;LOC tank_desc&gt;Tank" built="2" lost="1" killed="0" damagedealt="0.00" damagereceived="0.00" masscost="56.00" energycost="266.00" buildtime="280.00"/>
      </UnitStats>
      <SummaryStats>
        <Category type="AIR" built="0" killed="1"/>
        <Category type="LAND" built="2" killed="0"/>
        <Category type="NAVAL" built="0" killed="0"/>
        <Category type="ENGINEER" built="0" killed="0"/>
        <Category type="ARTILLERY" built="0" killed="0"/>
        <Category type="ANTIAIR" built="0" killed="0"/>
        <Category type="TRANSPORTATION" built="0" killed="0"/>
        <Category type="STRUCTURE" built="1" killed="0"/>
        <Category type="FACTORY" built="0" killed="0"/>
        <Category type="ENERGYPRODUCTION" built="1" killed="0"/>
        <Category type="MASSPRODUCTION" built="0" killed="0"/>
        <Category type="DEFENSE" built="0" killed="0"/>
        <Category type="TECH1" built="3" killed="0"/>
        <Category type="TECH2" built="0" killed="1"/>
        <Category type="TECH3" built="0" killed="0"/>
      </SummaryStats>
      <EconomyStats>
        <Energy produced="1234.57" consumed="1000.25" storage="4000.00"/>
        <Mass produced="650.50" consumed="100.00" storage="650.00"/>
      </EconomyStats>
  </Army>
  <Army index="1" name="ARMY_2">
      <UnitStats>
      </UnitStats>
      <SummaryStats>
        <Category type="AIR" built="0" killed="0"/>
        <Category type="LAND" built="0" killed="0"/>
        <Category type="NAVAL" built="0" killed="0"/>
        <Category type="ENGINEER" built="0" killed="0"/>
        <Category type="ARTILLERY" built="0" killed="0"/>
        <Category type="ANTIAIR" built="0" killed="0"/>
        <Category type="TRANSPORTATION" built="0" killed="0"/>
        <Category type="STRUCTURE" built="0" killed="0"/>
        <Category type="FACTORY" built="0" killed="0"/>
        <Category type="ENERGYPRODUCTION" built="0" killed="0"/>
        <Category type="MASSPRODUCTION" built="0" killed="0"/>
        <Category type="DEFENSE" built="0" killed="0"/>
        <Category type="TECH1" built="0" killed="0"/>
        <Category type="TECH2" built="0" killed="0"/>
        <Category type="TECH3" built="0" killed="0"/>
      </SummaryStats>
      <EconomyStats>
        <Energy produced="0.00" consumed="0.00" storage="0.00"/>
        <Mass produced="0.00" consumed="0.00" storage="0.00"/>
      </EconomyStats>
  </Army>
</GameStats>
)xml";

TEST_CASE("SubmitXMLArmyStats has the beat's sync build Moho's GameStats document, once",
          "[stats][gpgnet]") {
    StatsWorld w;
    lua_State* sim_L = w.sim_lua.raw();
    for (const char* bp : {
             "{BlueprintId = 'tank', Description = '<LOC tank_desc>Tank',"
             " Categories = {'LAND', 'MOBILE', 'TECH1'},"
             " Economy = {BuildCostMass = 56, BuildCostEnergy = 266, BuildTime = 280}}",
             "{BlueprintId = 'pgen', Description = 'Power Generator',"
             " Categories = {'STRUCTURE', 'ENERGYPRODUCTION', 'TECH1'},"
             " Economy = {BuildCostMass = 75, BuildCostEnergy = 750, BuildTime = 125.5}}",
             "{BlueprintId = 'bomber', Description = 'Bomber', Categories = {'AIR', 'TECH2'},"
             " Economy = {BuildCostMass = 210.333, BuildCostEnergy = 7000, BuildTime = 1600}}",
         }) {
        REQUIRE(w.sim_lua.do_string(std::string("return ") + bp).ok());
        w.store.register_blueprint(sim_L, osc::blueprints::BlueprintType::Unit, lua_gettop(sim_L));
        lua_pop(sim_L, 1);
    }
    w.store.expose_to_lua(sim_L);

    // The first army fields three tanks, loses one, builds a generator and
    // kills the second army's bomber; the second has only that loss, which
    // lists no unit
    osc::sim::ArmyBrain* tester = w.sim.get_army(0);
    tester->set_nickname("Tester");
    osc::sim::Unit tank;
    tank.set_blueprint_id("tank");
    osc::sim::Unit pgen;
    pgen.set_blueprint_id("pgen");
    for (int i = 0; i < 3; ++i) tester->record_unit_made(tank);
    tester->record_unit_removed(tank);
    tester->record_unit_lost("tank", 56, 266);
    tester->record_unit_made(pgen);
    tester->record_enemy_killed("bomber", 210.333, 7000, false);
    w.sim.get_army(1)->record_unit_lost("bomber", 210.333, 7000);
    for (const auto& [name, value] : {std::pair{"Economy_TotalProduced_Energy", 1234.567},
                                      std::pair{"Economy_TotalConsumed_Energy", 1000.25},
                                      std::pair{"Economy_MaxStorage_Energy", 4000.0},
                                      std::pair{"Economy_TotalProduced_Mass", 650.5},
                                      std::pair{"Economy_TotalConsumed_Mass", 99.999},
                                      std::pair{"Economy_MaxStorage_Mass", 650.0}}) {
        tester->set_stat(name, value);
    }
    lua_settop(sim_L, 0);
    CHECK(osc::lua::army_stats_xml(sim_L, w.sim) == kGameStats);
    CHECK(well_formed(kGameStats));
    CHECK(lua_gettop(sim_L) == 0);

    CHECK(run(w.sim_lua, "SubmitXMLArmyStats(1)")
              .find("Request that we submit xml army stats to gpg.net.\n  expected 0 args, but "
                    "got 1") != std::string::npos);
    REQUIRE(run(w.sim_lua, "Sync = {} function ResetSyncTable() Sync = {} end").empty());
    osc::lua::SyncQueue sync;
    sync.queue(sim_L, nullptr);
    CHECK(sync.deliver(sim_L, w.ui.raw()).empty());

    // Asked for in a beat: that beat's sync carries it, and only that one
    REQUIRE(run(w.sim_lua, "SubmitXMLArmyStats() SubmitXMLArmyStats()").empty());
    sync.queue(sim_L, nullptr);
    sync.queue(sim_L, nullptr);
    CHECK(sync.deliver(sim_L, w.ui.raw()) == kGameStats);
    CHECK(sync.deliver(sim_L, w.ui.raw()).empty());

    // With no beat queued, the delivery's own sync builds it
    REQUIRE(run(w.sim_lua, "SubmitXMLArmyStats()").empty());
    CHECK(sync.deliver(sim_L, w.ui.raw()) == kGameStats);
    CHECK(sync.deliver(sim_L, w.ui.raw()).empty());
    CHECK(lua_gettop(sim_L) == 0);
}

TEST_CASE("The GameStats document escapes the strings it puts in, where Moho's doesn't",
          "[stats][gpgnet]") {
    // A nickname comes from another peer, a blueprint from any mod; Moho's
    // document takes them as they are, and the engine's goes to GPGNet
    StatsWorld w;
    lua_State* sim_L = w.sim_lua.raw();
    REQUIRE(w.sim_lua
                .do_string(R"lua(return {BlueprintId = 'odd', Categories = {'LAND'},
                               Description = [[A&B <i>"q"</i> 'x']]})lua")
                .ok());
    w.store.register_blueprint(sim_L, osc::blueprints::BlueprintType::Unit, lua_gettop(sim_L));
    lua_pop(sim_L, 1);
    w.store.expose_to_lua(sim_L);
    osc::sim::ArmyBrain* tester = w.sim.get_army(0);
    tester->set_nickname("Tom & \"Jerry\" <x/> 'y'");
    osc::sim::Unit odd;
    odd.set_blueprint_id("odd");
    tester->record_unit_made(odd);

    const std::string xml = osc::lua::army_stats_xml(sim_L, w.sim);
    INFO(xml);
    CHECK(xml.find(
              R"(<Army index="0" name="Tom &amp; &quot;Jerry&quot; &lt;x/&gt; &apos;y&apos;">)") !=
          std::string::npos);
    CHECK(xml.find(R"(type="A&amp;B &lt;i&gt;&quot;q&quot;&lt;/i&gt; &apos;x&apos;" built="1")") !=
          std::string::npos);
    CHECK(well_formed(xml));
    // (Unescaped, it wouldn't be)
    CHECK_FALSE(well_formed(R"(<?xml version="1.0" ?>
<GameStats><Army name="Tom & "Jerry""></Army></GameStats>
)"));
}
