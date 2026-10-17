#include "command_sources_test.hpp"

#include "app/app.hpp"
#include "app/support.hpp"
#include "core/game_state.hpp"
#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_sync.hpp"
#include "render_probe.hpp"
#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit_command.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <string>
#include <vector>

namespace osc::test {

namespace {

/// The value of a Lua expression in `state`, as a number (0 on an error or
/// a non-number).
f64 number_of(lua::LuaState& state, const std::string& expr) {
    auto r = state.do_string("return tonumber(" + expr + ")");
    if (!r) {
        spdlog::error("command-sources-test Lua: {}", r.error().message);
        return 0;
    }
    lua_State* L = state.raw();
    const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);
    return v;
}

bool truth_of(lua::LuaState& state, const std::string& expr) {
    return number_of(state, "(" + expr + ") and 1 or 0") != 0;
}

std::string sources_of(const sim::SimState& sim, i32 army) {
    return fmt::format("{}", sim.army_command_sources(army));
}

} // namespace

void run_command_sources_test(app::Engine& e) {
    spdlog::info("=== COMMAND SOURCES TEST: who commands an army ===");
    Tally t;
    lua_State* uL = e.ui_lua_state.raw();
    lua_pushstring(uL, "__osc_command_sources_test_map");
    lua_pushstring(uL, e.map_path.c_str());
    lua_rawset(uL, LUA_GLOBALSINDEX);
    // Two players and an AI on one team (armies 0, 1, 2: the players'
    // sources 0 and 1), an AI against them (army 3)
    if (auto r = e.ui_lua_state.do_string(R"(
            local scenario = rawget(_G, '__osc_command_sources_test_map')
            LaunchSinglePlayerSession({
                GameOptions = { ScenarioFile = scenario, CommonArmy = 'Union',
                                Victory = 'sandbox' },
                PlayerOptions = {
                    [1] = { Human = true, PlayerName = 'One', Faction = 1, Team = 2,
                            StartSpot = 1 },
                    [2] = { Human = true, PlayerName = 'Two', Faction = 1, Team = 2,
                            StartSpot = 2 },
                    [3] = { Human = false, PlayerName = 'Ally', AIPersonality = 'adaptive',
                            Faction = 2, Team = 2, StartSpot = 3 },
                    [4] = { Human = false, PlayerName = 'Enemy', AIPersonality = 'adaptive',
                            Faction = 3, Team = 3, StartSpot = 4 },
                },
            })
        )");
        !r) {
        osc::test_status::fail("command-sources-test: launching: {}", r.error().message);
        return;
    }
    double accumulator = 0.0;
    if (!app::execute_reload_sequence(
            e.sim_lua_state, e.sim_state, e.ui_lua_state, e.vfs, e.store, e.loader, e.config,
            e.scenario_meta, e.game_state_mgr, nullptr, nullptr, nullptr, nullptr, nullptr,
            app::new_game_seed(e.seed_arg, /*reproducible=*/true), accumulator, e.map_path)) {
        osc::test_status::fail("command-sources-test: the launch's reload failed");
        return;
    }
    sim::SimState& sim = *e.sim_state;
    lua::LuaState& sim_lua = *e.sim_lua_state;
    uL = e.ui_lua_state.raw();
    lua_State* sL = sim_lua.raw();
    const auto beat = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) sim.tick();
        lua::sync_beat(sL, uL);
    };
    const auto end_source = [&](u32 source) {
        sim::SimCallbackEntry gone;
        gone.func_name = sim::kCommandSourceTerminatedCallback;
        gone.args["Source"] = static_cast<f64>(source);
        sim.schedule_callback(sim::kEngineSource, gone);
        beat(2);
    };
    // A UI request in a world session (headless, there is no game interface)
    const auto ask_focus = [&](int army) {
        lua_pushstring(uL, core::kWorldUiActiveKey);
        lua_pushboolean(uL, 1);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        const bool asked =
            static_cast<bool>(e.ui_lua_state.do_string(fmt::format("SetFocusArmy({})", army)));
        lua_pushstring(uL, core::kWorldUiActiveKey);
        lua_pushnil(uL);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        beat(1);
        return asked;
    };
    beat(10); // the armies' units are in
    // FAF's simInit has union and common armies; retail's knows no such option
    const bool faf = truth_of(sim_lua, "rawget(_G, 'MergeArmyPair')");
    spdlog::info("command-sources-test: {}'s scripts", faf ? "FAF" : "retail");

    // Test 1: each player's army takes its commands, the AIs' no one's;
    // under FAF's union army each player commands all three allies
    {
        const std::string mine = faf ? "[0, 1]" : "[0]";
        const std::string two = faf ? "[0, 1]" : "[1]";
        const std::string ally = faf ? "[0, 1]" : "[]";
        t.check(sim.command_sources_known() && sources_of(sim, 0) == mine &&
                    sources_of(sim, 1) == two && sources_of(sim, 2) == ally &&
                    sources_of(sim, 3) == "[]",
                fmt::format("Test 1: command sources {} {} {} {} (want {} {} {} [])",
                            sources_of(sim, 0), sources_of(sim, 1), sources_of(sim, 2),
                            sources_of(sim, 3), mine, two, ally));
    }

    // Test 2: the UI sees them (1-based), as FAF's score board offers them
    const f64 ally_listed = number_of(e.ui_lua_state, "table.getn(GetArmiesTable().armiesTable[3]"
                                                      ".authorizedCommandSources)");
    const f64 own_listed =
        number_of(e.ui_lua_state, "GetArmiesTable().armiesTable[2].authorizedCommandSources[1]");
    t.check(own_listed == (faf ? 1 : 2) && ally_listed == (faf ? 2 : 0),
            fmt::format("Test 2: GetArmiesTable lists source {} first for player two, {} "
                        "sources for the AI ally",
                        own_listed, ally_listed));

    // Test 3: player one's order reaches the AI ally's units only under union
    const f64 ally_unit =
        number_of(sim_lua, "ArmyBrains[3]:GetListOfUnits(categories.ALLUNITS, false)[1]"
                           ":GetEntityId()");
    const f64 enemy_unit =
        number_of(sim_lua, "ArmyBrains[4]:GetListOfUnits(categories.ALLUNITS, false)[1]"
                           ":GetEntityId()");
    sim::UnitCommand stop;
    stop.type = sim::CommandType::Stop;
    sim.schedule_command(0, {static_cast<u32>(ally_unit), static_cast<u32>(enemy_unit)}, stop,
                         true);
    beat(2);
    std::vector<u32> applied;
    const auto& recorded = sim.recorded_replay().commands;
    for (auto it = recorded.rbegin(); it != recorded.rend(); ++it)
        if (it->source == 0 && it->command.type == sim::CommandType::Stop) {
            applied = it->unit_ids;
            break;
        }
    const std::vector<u32> want_applied =
        faf ? std::vector<u32>{static_cast<u32>(ally_unit)} : std::vector<u32>{};
    t.check(ally_unit != 0 && enemy_unit != 0 && applied == want_applied,
            fmt::format("Test 3: player one's Stop to the ally's {} and the enemy's {} reached "
                        "{} (want {})",
                        ally_unit, enemy_unit, applied, want_applied));

    // Test 4: player one may watch the AI ally only when it commands it
    const bool asked = ask_focus(3);
    const int watching = lua::focus_army(uL);
    t.check(
        asked && watching == (faf ? 2 : 0),
        fmt::format("Test 4: asked to watch the AI ally, player one watches army {}", watching));

    if (!faf) {
        // Test 5: player two drops: retail's AbandonedByPlayer defeats its army
        end_source(1);
        const bool two_out = sim.army_at(1)->is_defeated();
        const bool one_in = !sim.army_at(0)->is_defeated();
        t.check(sources_of(sim, 1) == "[]" && two_out && one_in,
                fmt::format("Test 5: player two's source ended: its army has {}, defeated {}; "
                            "player one's still in {}",
                            sources_of(sim, 1), two_out, one_in));
        spdlog::info("Command sources test: {}/{} passed", t.pass, t.pass + t.fail);
        return;
    }

    // Test 5: FAF's common army merges player two into player one
    // (MergeArmyPair(into, from, its commander, humanIndex), once the
    // commander is in): the control, units -- its commander too, which
    // FAF's exe gives (FA-Binary-Patches hooks/HTransferACUs.cpp) -- and,
    // for one watching it, the focus go to the army merged into.
    (void)ask_focus(2);
    const bool watched_two = lua::focus_army(uL) == 1;
    // Player two has an engineer to hand over besides its commander. Asked
    // without noRestrictions, FAF's Lua keeps the commander (SimHooks.lua).
    const bool merging = static_cast<bool>(sim_lua.do_string(R"(
        local acu = ArmyBrains[2]:GetListOfUnits(categories.COMMAND, false)[1]
        __osc_commander_kept = ChangeUnitArmy(acu, 1) == nil
        local x, y, z = unpack(acu:GetPosition())
        CreateUnitHPR('uel0105', 'ARMY_2', x + 4, y, z, 0, 0, 0)
        ForkThread(MergeArmyPair, 1, 2, acu, 1)
    )"));
    const bool kept = truth_of(sim_lua, "__osc_commander_kept");
    for (int i = 0; i < 60 && sim.army_has_command_source(1, 1); ++i) beat(10);
    beat(2);
    const f64 left = number_of(sim_lua, "table.getn(ArmyBrains[2]:GetListOfUnits("
                                        "categories.ALLUNITS, false))");
    const f64 given = number_of(sim_lua, "table.getn(ArmyBrains[1]:GetListOfUnits("
                                         "categories.ENGINEER - categories.COMMAND, false))");
    const f64 commanders = number_of(sim_lua, "table.getn(ArmyBrains[1]:GetListOfUnits("
                                              "categories.COMMAND, false))");
    const int focus = lua::focus_army(uL);
    t.check(watched_two && merging && kept && left == 0 && given >= 1 && commanders == 2 &&
                sources_of(sim, 0) == "[0, 1]" && sources_of(sim, 1) == "[0]" && focus == 0,
            fmt::format("Test 5: merged: player one's army {} with {} engineers and {} "
                        "commanders (kept when asked plainly {}), player two's {} with {} units "
                        "left, focus {} (was on two {})",
                        sources_of(sim, 0), given, commanders, kept, sources_of(sim, 1), left,
                        focus, watched_two));

    // Test 6: union control upon disconnect: player one drops; player two's
    // old army, left with no player, is abandoned, and FAF's brain shares it
    // with its allies (ShareControlWithAllies) rather than defeat it
    const bool option_set = static_cast<bool>(
        sim_lua.do_string("ScenarioInfo.Options.CommonArmy = 'UnionWhenDisconnected'"));
    end_source(0);
    const bool shared = option_set && truth_of(sim_lua, "ArmyBrains[2].AbandonedAt ~= nil") &&
                        !sim.army_at(1)->is_defeated();
    t.check(shared && sources_of(sim, 0) == "[1]" && sources_of(sim, 1) == "[0]" &&
                sources_of(sim, 2) == "[1]",
            fmt::format("Test 6: player one's source ended: two's old army abandoned and "
                        "shared {} ({}), one's {}, the AI ally's {}",
                        shared, sources_of(sim, 1), sources_of(sim, 0), sources_of(sim, 2)));

    // Test 7: player two drops too: no ally is left to share with, so FAF's
    // brains defeat the armies (the AI ally's too: no player commands it)
    end_source(1);
    const bool one_defeated = truth_of(sim_lua, "ArmyBrains[1].AbandonedAt ~= nil and "
                                                "ArmyBrains[1].Status == 'Defeat'");
    const bool ally_defeated = truth_of(sim_lua, "ArmyBrains[3].Status == 'Defeat'");
    t.check(one_defeated && ally_defeated,
            fmt::format("Test 7: player two's source ended: player one's army defeated {}, the "
                        "AI ally's {}",
                        one_defeated, ally_defeated));

    spdlog::info("Command sources test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
