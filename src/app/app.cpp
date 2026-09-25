#include "app/app.hpp"
#include "app/support.hpp"
#include "core/fixed_step.hpp"
#include "core/image.hpp"
#include "core/test_status.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/tick_clock.hpp"
#include "core/types.hpp"
#include "platform/crash_handler.hpp"
#include "platform/game_install.hpp"
#include "platform/paths.hpp"
#include "lua/lua_state.hpp"
#include "lua/init_loader.hpp"
#include "lua/session_manager.hpp"
#include "lua/special_files.hpp"
#include "lua/sim_loader.hpp"
#include "lua/script_loader.hpp"
#include "lua/binding_coverage.hpp"
#include "lua/scenario_loader.hpp"
#include "lua/sim_bindings.hpp"
#include "vfs/virtual_file_system.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/sim_state.hpp"
#include "sim/bone_cache.hpp"
#include "sim/anim_cache.hpp"
#include "map/terrain.hpp"
#include "audio/sound_manager.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "lua/beat_system.hpp"
#include "lua/factory_queue.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "renderer/renderer.hpp"
#include "renderer/input_handler.hpp"
#include "ui/world_view.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/net_transport.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/replay.hpp"
#include "sim/build_info.hpp"
#include "sim/game_setup.hpp"
#include "sim/world_snapshot.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include "lua/lan_lobby.hpp"
#include "lua/lan_dialog_ui.hpp"
#include "lua/smoke_test.hpp"

extern "C" {
#include <lua.h>
}

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>
#include <spdlog/spdlog.h>

namespace osc::app {

// ── Engine global Lua C functions (file-static, outside run) ──
// Note: GetCurrentUIState and WorldIsLoading moved to moho_bindings.cpp (M144c)

static int l_FlushEvents(lua_State*) { return 0; }

/// SessionIsReplay() for the UI: whether the game is a replay being played
/// (retail's UI then hides orders and shows the replay controls). Only UI
/// scripts ask it; the sim's own answer stays false, so a replay can't
/// change what the game does.
static int l_SessionIsReplay(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* sim = static_cast<const osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, sim && sim->playback() ? 1 : 0);
    return 1;
}

static int l_SessionGetScenarioInfo(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L);
    if (!sim || !sim->terrain()) return 1;

    lua_pushstring(L, "name");
    lua_pushstring(L, "Skirmish");
    lua_rawset(L, -3);

    lua_pushstring(L, "map");
    lua_pushstring(L, "");
    lua_rawset(L, -3);

    lua_pushstring(L, "size");
    lua_pushnumber(L, sim->terrain()->map_width());
    lua_rawset(L, -3);

    lua_pushstring(L, "PlayableArea");
    lua_newtable(L);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, sim->terrain()->map_width()); lua_rawseti(L, -2, 3);
    lua_pushnumber(L, sim->terrain()->map_height()); lua_rawseti(L, -2, 4);
    lua_rawset(L, -3);

    return 1;
}

static int l_GetEconomyTotals(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int army = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);

    lua_newtable(L); // result table
    // Observers (focus army -1) and a missing sim get the same shape with
    // zeros, as Moho's does: the economy bar reads every field every beat.
    auto* brain = sim ? sim->get_army(army) : nullptr;
    static const osc::sim::EconomyState kNoEconomy{};
    const auto& econ = brain ? brain->economy() : kNoEconomy;

    // Helper: push a subtable with MASS and ENERGY keys
    auto push_resource_subtable = [&](const char* name, osc::f64 mass_val, osc::f64 energy_val) {
        lua_pushstring(L, name);
        lua_newtable(L);
        lua_pushstring(L, "MASS");
        lua_pushnumber(L, mass_val);
        lua_rawset(L, -3);
        lua_pushstring(L, "ENERGY");
        lua_pushnumber(L, energy_val);
        lua_rawset(L, -3);
        lua_rawset(L, -3); // set subtable on result
    };

    push_resource_subtable("income", econ.mass.income, econ.energy.income);
    push_resource_subtable("lastUseActual",
        brain ? brain->get_economy_usage("MASS") : 0.0,
        brain ? brain->get_economy_usage("ENERGY") : 0.0);
    push_resource_subtable("lastUseRequested", econ.mass.requested, econ.energy.requested);
    push_resource_subtable("maxStorage", econ.mass.max_storage, econ.energy.max_storage);
    push_resource_subtable("stored", econ.mass.stored, econ.energy.stored);
    push_resource_subtable("reclaimed", 0.0, 0.0); // TODO: track cumulative reclaim

    return 1;
}

static int l_GetSimTicksPerSecond(lua_State* L) {
    lua_pushnumber(L, 10.0);
    return 1;
}

static int l_ui_IsAlly(lua_State* L) {
    int army1 = static_cast<int>(lua_tonumber(L, 1)) - 1; // 1-based Lua → 0-based C++
    int army2 = static_cast<int>(lua_tonumber(L, 2)) - 1;

    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!sim) { lua_pushboolean(L, 0); return 1; }
    auto* brain = sim->get_army(army1);
    if (!brain) { lua_pushboolean(L, 0); return 1; }

    lua_pushboolean(L, brain->is_ally(army2) ? 1 : 0);
    return 1;
}

static int l_GetArmiesTable(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L); // result table

    // Build the armiesTable array
    lua_pushstring(L, "armiesTable");
    lua_newtable(L); // armiesTable array

    if (sim) {
        for (size_t i = 0; i < sim->army_count(); ++i) {
            auto* brain = sim->army_at(i);
            if (!brain) continue;

            lua_newtable(L); // per-army entry

            lua_pushstring(L, "nickname");
            lua_pushstring(L, brain->nickname().c_str());
            lua_rawset(L, -3);

            lua_pushstring(L, "ArmyName");
            lua_pushstring(L, brain->name().c_str());
            lua_rawset(L, -3);

            lua_pushstring(L, "armyIndex");
            lua_pushnumber(L, static_cast<int>(i));
            lua_rawset(L, -3);

            lua_pushstring(L, "human");
            lua_pushboolean(L, brain->is_human() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "civilian");
            lua_pushboolean(L, brain->is_civilian() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "outOfGame");
            lua_pushboolean(L, brain->is_defeated() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "faction");
            lua_pushnumber(L, brain->faction());
            lua_rawset(L, -3);

            // Color as ARGB hex string (e.g. "ffFF8000")
            {
                char color_buf[16];
                std::snprintf(color_buf, sizeof(color_buf), "ff%02X%02X%02X",
                    brain->color_r(), brain->color_g(), brain->color_b());
                lua_pushstring(L, "color");
                lua_pushstring(L, color_buf);
                lua_rawset(L, -3);
            }

            lua_pushstring(L, "showScore");
            lua_pushboolean(L, brain->is_civilian() ? 0 : 1);
            lua_rawset(L, -3);

            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }

    lua_rawset(L, -3); // result.armiesTable = array

    // focusArmy field
    lua_pushstring(L, "focusArmy");
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int focus = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    lua_pushnumber(L, focus + 1); // 1-based for Lua
    lua_rawset(L, -3);

    // numArmies field
    lua_pushstring(L, "numArmies");
    lua_pushnumber(L, sim ? static_cast<int>(sim->army_count()) : 0);
    lua_rawset(L, -3);

    return 1;
}

static void print_usage() {
    // The option table is laid out by hand.
    // clang-format off
    std::cout << "OpenSupCom v0.1.0\n"
              << "Open-source engine reimplementation for Supreme Commander: "
                 "Forged Alliance\n\n"
              << "Usage:\n"
              << "  opensupcom [options]\n\n"
              << "Options:\n"
              << "  --init <path>      Path to init.lua / init_faf.lua\n"
              << "  --fa-path <path>   Path to FA installation directory\n"
              << "  --faf-data <path>  Path to FAF data directory\n"
              << "  --print-install    Show which FA install would be used and exit\n"
              << "  --screenshot <png> Render on a fixed clock, save frame N, exit\n"
              << "  --screenshot-frame <N>  Frame to capture (default 120)\n"
              << "  --camera <x>,<z>,<d>    Initial camera target and distance\n"
              << "  --legacy-hud       Draw the C++ HUD placeholders over FA's game interface\n"
              << "  --prefs <path>     Game.prefs to use (default: the user's config dir;\n"
              << "                     tests and captures keep preferences in memory)\n"
              << "  --golden <name>    Capture like --screenshot, compare to golden image\n"
              << "  --golden-update    Record the golden image instead of comparing\n"
              << "  --binding-coverage <file>  Report engine API the scripts call but\n"
              << "                     the engine lacks (needs --map)\n"
              << "  --binding-baseline <file>  With --binding-coverage: fail on gaps\n"
              << "                     not listed in this baseline\n"
              << "  --dump-threads     At exit, list where each sim script thread waits\n"
              << "  --ai-armies <n>    With --ai-skirmish: number of AI armies (default 2)\n"
              << "  --map <vfs-path>   VFS path to *_scenario.lua\n"
              << "  --ticks <n>        Number of sim ticks to run (default: 100)\n"
              << "  --seed <n>         The game's random seed (default: fixed for tests and\n"
              << "                     headless runs, fresh for an interactive game)\n"
              << "  --checksum-trace <f>  Write each tick's sync checksum and its parts\n"
              << "  --entity-trace <f>    Write every entity's synced state each tick\n"
              << "  --entity-trace-ticks <from>-<to>  ...only for these ticks\n"
              << "  --rng-trace <f>       Write each random draw and the script that made it\n"
              << "  --rng-trace-ticks <from>-<to>  ...only for these ticks\n"
              << "  --record <file>    Record the game as a replay, written when the run ends\n"
              << "  --watch <file>     Watch a replay in the game\n"
              << "  --user-dir <dir>   Replays and saved games folder (default: FA's user folder\n"
              << "                     for an interactive game, else a temporary one)\n"
              << "  --replay-flow-test Offscreen: open the first listed replay as retail's\n"
              << "                     replay dialog does, and watch it to its end\n"
              << "  --replay <file>    Play a recorded game headlessly, checking every tick's\n"
              << "                     checksum against the recording (exit 1 on divergence)\n"
              << "  --scripted-orders  With --ai-skirmish: army 1 also takes a player's\n"
              << "                     orders (moves, pauses, fire states, stops)\n"
              << "  --profile          Enable performance profiling (prints summary at exit)\n"
              << "  --instrument       Interactive instrumented mode (smoke report on exit)\n"
              << "  --help             Show this help message\n";
    // clang-format on
}

static osc::lua::InitConfig parse_args(int argc, char* argv[], const TestModes* tests) {
    osc::platform::GameInstallHints hints;
    bool print_install = false;

    for (int i = 1; i < argc; i++) {
        auto take_path = [&](std::optional<osc::fs::path>& out) {
            const char* value = argv[++i];
            if (*value) out = value; // "" means "not given"
        };
        if (std::strcmp(argv[i], "--init") == 0 && i + 1 < argc) {
            take_path(hints.init_file);
        } else if (std::strcmp(argv[i], "--fa-path") == 0 && i + 1 < argc) {
            take_path(hints.fa_path);
        } else if (std::strcmp(argv[i], "--faf-data") == 0 && i + 1 < argc) {
            take_path(hints.faf_data_path);
        } else if (std::strcmp(argv[i], "--print-install") == 0) {
            print_install = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage();
            if (tests) tests->print_usage();
            std::exit(0);
        }
    }

    auto search = osc::platform::locate_game_install(
        hints, osc::platform::system_env());

    if (print_install) {
        if (search.install) {
            std::cout << "source="    << search.install->source << "\n"
                      << "fa_path="   << search.install->fa_path.string() << "\n"
                      << "init_file=" << search.install->init_file.string() << "\n"
                      << "faf_data="  << search.install->faf_data_path.string() << "\n";
        } else {
            std::cout << "No Supreme Commander: Forged Alliance installation found.\n";
        }
        for (const auto& where : search.searched) {
            std::cout << "searched " << where << "\n";
        }
        std::exit(search.install ? 0 : 1);
    }

    osc::lua::InitConfig config;
    if (search.install) {
        config.fa_path = search.install->fa_path;
        config.init_file = search.install->init_file;
        config.faf_data_path = search.install->faf_data_path;
        spdlog::info("Game install ({}): {}", search.install->source,
                     config.fa_path.string());
    } else {
        for (const auto& where : search.searched) {
            spdlog::info("Searched for FA: {}", where);
        }
    }
    return config;
}

static osc::u32 parse_ticks_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || val < 0 || val > 1'000'000) {
                spdlog::error("Invalid --ticks value: {}", argv[i]);
                std::exit(1);
            }
            return static_cast<osc::u32>(val);
        }
    }
    return 0; // 0 = no explicit tick count → windowed mode
}

static std::string parse_map_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            return argv[++i];
        }
    }
    return {};
}

bool parse_flag(int argc, char* argv[], const char* flag) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

std::string parse_string_arg(int argc, char* argv[], const char* flag, const char* default_val) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0 && i + 1 < argc) {
            return argv[++i];
        }
    }
    return default_val;
}

int finish_test_run(const char* mode, osc::u32 smoke_issues) {
    if (smoke_issues > 0) {
        osc::test_status::record_failure(
            fmt::format("{} smoke issue(s) reported (see smoke report)", smoke_issues));
    }
    const int failures = osc::test_status::failure_count();
    if (failures == 0) {
        spdlog::info("=== {}: PASS ===", mode);
        return 0;
    }
    spdlog::error("=== {}: FAIL ({} failed check(s)) ===", mode, failures);
    for (const auto& message : osc::test_status::failure_messages()) {
        spdlog::error("  - {}", message);
    }
    return 1;
}

void detach_ui_from_sim(lua_State* uiL) {
    lua_pushstring(uiL, "osc_sim_state");
    lua_pushnil(uiL);
    lua_rawset(uiL, LUA_REGISTRYINDEX);
}

// ── Reload sequence: tears down old sim, creates fresh Lua VM + SimState,
//    reloads blueprints/scenario, boots sim, rebuilds renderer scene. ──
// Returns true on success, false on critical failure.
/// Give a sim (and its Lua state) the application's sound engine.
static void attach_sound(osc::lua::LuaState& sim_lua, osc::sim::SimState& sim,
                         osc::audio::SoundManager* sound) {
    lua_State* L = sim_lua.raw();
    lua_pushstring(L, "osc_sound_manager");
    if (sound) lua_pushlightuserdata(L, sound);
    else lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    sim.set_sound_manager(sound);
}

/// --checksum-trace <file>: every game's sims write their per-tick checksum
/// here (see SimState::set_checksum_trace); null when not asked for.
static std::ofstream* g_checksum_trace = nullptr;

/// --entity-trace <file> [--entity-trace-ticks <from>-<to>]: every entity's
/// synced state at those ticks (see SimState::set_entity_trace).
static std::ofstream* g_entity_trace = nullptr;
static osc::u32 g_entity_trace_from = 0;
static osc::u32 g_entity_trace_to = 0xFFFFFFFFu;

/// --rng-trace <file> [--rng-trace-ticks <from>-<to>]: every random draw and
/// the script that made it (see SimState::set_rng_trace).
static std::ofstream* g_rng_trace = nullptr;
static osc::u32 g_rng_trace_from = 0;
static osc::u32 g_rng_trace_to = 0xFFFFFFFFu;

/// --record <file>: each game records (SimState::set_recording), and the
/// run writes the last one's replay here as it ends. Empty: no recording.
static std::string g_record_path;

/// Write `sim`'s recording to `path`.
static bool write_recording(const osc::sim::SimState& sim, const std::string& path) {
    return osc::lua::write_replay_file(sim.recorded_replay(), path);
}

/// A replay file that can start its game, or nothing (the reason logged).
static std::optional<osc::sim::Replay> load_replay(const std::string& path) {
    auto replay = osc::lua::read_replay_file(path);
    if (replay && replay->build != osc::sim::build_id()) {
        spdlog::warn("Replay: recorded by build {}, playing on {}; a different build may "
                     "play it differently",
                     replay->build, osc::sim::build_id());
    }
    return replay;
}

/// --scripted-orders: army 1 also takes a player's orders, routed as a
/// player's so a recording holds them: moves, now and then a fire state or
/// a pause (SimCallbacks) and a Stop. Picked from their own random stream,
/// never the sim's.
static void issue_scripted_orders(osc::sim::SimState& sim, osc::sim::SimRandom& rng,
                                  osc::u32 tick) {
    if (tick % 40 != 20 || !sim.terrain()) return;
    std::vector<osc::u32> movers, all; // army 1's live units, in id order
    sim.entity_registry().for_each([&](const osc::sim::Entity& e) {
        if (!e.is_unit() || e.destroyed() || e.army() != 0) return;
        all.push_back(e.entity_id());
        if (static_cast<const osc::sim::Unit&>(e).has_command_cap("RULEUCC_Move"))
            movers.push_back(e.entity_id());
    });
    if (all.empty()) return;
    auto any = [&](const std::vector<osc::u32>& ids) {
        return ids[static_cast<size_t>(rng.next_int(0, static_cast<osc::i64>(ids.size()) - 1))];
    };
    osc::sim::UnitCommand cmd;
    std::vector<osc::u32> ids;
    switch ((tick / 40) % 6) {
    case 3: { // a fire state
        osc::sim::SimCallbackEntry cb;
        cb.func_name = osc::sim::kUnitSettingCallback;
        cb.args["Setting"] = std::string("FireState");
        cb.args["Value"] = static_cast<osc::f64>(rng.next_int(0, 2));
        cb.unit_ids = {any(all)};
        sim.submit_callback(std::move(cb));
        return;
    }
    case 4: { // pause or resume
        const osc::u32 id = any(all);
        const auto* u = static_cast<const osc::sim::Unit*>(sim.entity_registry().find(id));
        osc::sim::SimCallbackEntry cb;
        cb.func_name = osc::sim::kUnitSettingCallback;
        cb.args["Setting"] = std::string("Paused");
        cb.args["Value"] = !u->is_paused();
        cb.unit_ids = {id};
        sim.submit_callback(std::move(cb));
        return;
    }
    case 5: // stop
        cmd.type = osc::sim::CommandType::Stop;
        ids = {any(all)};
        break;
    default: { // move a few units somewhere
        if (movers.empty()) return;
        cmd.type = osc::sim::CommandType::Move;
        const auto w = static_cast<double>(sim.terrain()->map_width());
        const auto h = static_cast<double>(sim.terrain()->map_height());
        cmd.target_pos = {static_cast<osc::f32>(rng.next_double() * w), 0.0f,
                          static_cast<osc::f32>(rng.next_double() * h)};
        for (int k = 0; k < 3; ++k) ids.push_back(any(movers));
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        break;
    }
    }
    sim.set_human_input_active(true);
    sim.route_command(ids, cmd, true);
    sim.set_human_input_active(false);
}

/// --replay: play the recorded game to its end, checking the checksum after
/// every tick against the recording's. 0 when they match throughout.
static int play_replay(osc::sim::SimState& sim, const osc::sim::Replay& replay) {
    osc::sim::ReplayPlayback playback(replay);
    playback.start(sim);
    spdlog::info("Replay: {} commands over {} ticks", replay.commands.size(), replay.final_tick);
    while (!playback.finished(sim)) {
        sim.tick();
        if (!playback.check(sim)) {
            const osc::u32 tick = playback.diverged_at();
            spdlog::error("Replay diverged at tick {}: checksum {:08x}, recorded {:08x}", tick,
                          sim.compute_sync_checksum(),
                          replay.checksums[tick - replay.checksum_from]);
            std::printf("REPLAY diverged tick=%u\n", tick);
            return 1;
        }
    }
    spdlog::info("Replay: matched the recording at every tick");
    std::printf("REPLAY ok ticks=%u checksum=%08x\n", sim.tick_count(),
                sim.compute_sync_checksum());
    return 0;
}

/// The random seed of a new game: `--seed` when given; else a fixed one when
/// the run must repeat (tests, headless runs, captures); else a fresh one.
/// A multiplayer session then replaces it with the seed its peers share.
osc::u64 new_game_seed(const std::string& seed_arg, bool reproducible) {
    if (!seed_arg.empty()) return std::strtoull(seed_arg.c_str(), nullptr, 0);
    if (reproducible) return osc::sim::SimRandom::kDefaultSeed;
    std::random_device rd;
    return (static_cast<osc::u64>(rd()) << 32) ^ rd();
}

/// The seed a launch boots its sim with. A multiplayer launch uses the seed
/// the lobby shared: every peer must roll the same numbers from the first
/// line of boot (scenario scripts, BeginSession's AI setup), not only once
/// the session attaches afterwards.
static osc::u64 launch_seed(const std::string& seed_arg, bool reproducible) {
    const auto& mp = osc::lua::mp_net_state();
    if (mp.transport_ready) return mp.seed;
    return new_game_seed(seed_arg, reproducible);
}

/// The world as it is drawn: the sim's last two ticks, and how far the frame
/// is between them. Every tick lands here, whoever runs it (the loop, the
/// lockstep session), so interpolation follows the ticks that really came.
struct WorldInterp {
    osc::sim::WorldHistory history;
    osc::TickClock clock{osc::sim::SimState::SECONDS_PER_TICK};

    /// Follow a new sim's ticks from its first one. Must outlive `sim`.
    void attach(osc::sim::SimState& sim) {
        history.clear();
        clock.reset();
        sim.set_tick_observer([this](const osc::sim::SimState& s) {
            history.capture(s);
            clock.on_tick();
        });
    }

    osc::sim::FrameView view() const {
        return {&history.prev(), &history.cur(), clock.alpha()};
    }
};

bool execute_reload_sequence(std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
                             std::unique_ptr<osc::sim::SimState>& sim_state,
                             osc::lua::LuaState& ui_lua_state, osc::vfs::VirtualFileSystem& vfs,
                             osc::blueprints::BlueprintStore& store, osc::lua::InitLoader& loader,
                             const osc::lua::InitConfig& config,
                             osc::lua::ScenarioMetadata& scenario_meta,
                             osc::GameStateManager& game_state_mgr,
                             osc::renderer::Renderer* renderer,            // nullable for headless
                             osc::renderer::InputHandler* input_handler,   // nullable for headless
                             std::unordered_set<osc::u32>* prev_selection, // nullable for headless
                             WorldInterp* world_interp,                    // nullable for headless
                             osc::u64 seed, // the new game's random seed
                             double& sim_accumulator, const std::string& launch_scenario,
                             const osc::sim::Replay* replay) { // a replay to play instead

    lua_State* uiL = ui_lua_state.raw();

    // 1. GPU fence — ensure no in-flight work
    if (renderer) renderer->clear_scene();

    // 2. Destroy old SimState and sim Lua state. A multiplayer LockstepSession
    // holds a reference to the SimState, so drop any stale session first (but
    // keep the transport — HostGame/JoinGame created it before launch and
    // mp_attach_session rebuilds the session over it once the fresh sim exists).
    osc::lua::mp_net_state().session.reset();
    detach_ui_from_sim(uiL);
    sim_state.reset();
    sim_lua_state.reset();

    // 3. Create fresh sim Lua state
    sim_lua_state = std::make_unique<osc::lua::LuaState>();
    sim_lua_state->set_vfs(&vfs);
    sim_lua_state->set_blueprint_store(&store);

    // 4. Run init sequence on new sim state (polyfills, config, class, import)
    auto reinit_result = loader.execute_init(*sim_lua_state, config, vfs);
    if (!reinit_result) {
        spdlog::error("Reload init failed: {}", reinit_result.error().message);
        return false;
    }

    // 5. Rebind BlueprintStore to new Lua state and reload blueprints
    store.rebind(sim_lua_state->raw());
    auto rebp_result = loader.load_blueprints(*sim_lua_state, vfs, store);
    if (!rebp_result) {
        spdlog::error("Reload blueprint load failed: {}", rebp_result.error().message);
        return false;
    }

    // 6. Create fresh SimState
    sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
    if (replay) seed = replay->setup.seed;
    sim_state->set_seed(seed);
    sim_state->set_checksum_trace(g_checksum_trace);
    sim_state->set_entity_trace(g_entity_trace, g_entity_trace_from, g_entity_trace_to);
    sim_state->set_rng_trace(g_rng_trace, g_rng_trace_from, g_rng_trace_to);
    spdlog::info("Game seed {:#018x}", seed);

    // 7. Audio (the application's engine, kept in the UI state), bone
    // cache, anim cache
    {
        lua_pushstring(uiL, "osc_sound_manager");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        auto* sound = static_cast<osc::audio::SoundManager*>(lua_touserdata(uiL, -1));
        lua_pop(uiL, 1);
        attach_sound(*sim_lua_state, *sim_state, sound);
    }
    if (world_interp) world_interp->attach(*sim_state);
    sim_state->set_bone_cache(
        std::make_unique<osc::sim::BoneCache>(&vfs, &store));
    sim_state->set_anim_cache(
        std::make_unique<osc::sim::AnimCache>(&vfs));

    // The game's setup: the lobby's sessionConfig (which of the scenario's
    // armies play, who plays each, the options), the scenario and the seed.
    // Moho creates only the filled slots' armies, and retail InitializeArmies
    // spawns an ACU for every army ListArmies() returns -- an army without a
    // brain then runs its commander's scripts against no brain at all.
    osc::sim::GameSetup setup;
    if (replay) {
        setup = replay->setup; // the recorded game's own
    } else {
        lua_pushstring(uiL, "__osc_front_end_data");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        auto* fed = static_cast<osc::FrontEndData*>(lua_touserdata(uiL, -1));
        lua_pop(uiL, 1);
        if (fed) {
            const int top = lua_gettop(uiL);
            fed->get(uiL, "sessionConfig");
            if (lua_istable(uiL, -1)) setup = osc::lua::read_session_config(uiL, lua_gettop(uiL));
            lua_settop(uiL, top);
        }
    }
    if (!replay) {
        setup.scenario = launch_scenario;
        setup.seed = seed;
    }

    // 8. Load scenario from selected map
    osc::lua::ScenarioLoader new_scenario_loader;
    auto new_meta_result = new_scenario_loader.load_scenario(
        *sim_lua_state, vfs, launch_scenario, *sim_state);
    if (!new_meta_result) {
        spdlog::error("Reload scenario failed: {}",
                      new_meta_result.error().message);
    } else {
        scenario_meta = new_meta_result.value();
        const size_t army_limit =
            setup.army_count > 0
                ? std::min(static_cast<size_t>(setup.army_count), scenario_meta.armies.size())
                : scenario_meta.armies.size();
        for (size_t i = 0; i < army_limit; ++i) {
            sim_state->add_army(scenario_meta.armies[i], scenario_meta.armies[i]);
        }
    }
    if (sim_state->army_count() == 0) {
        sim_state->add_army("ARMY_1", "Player");
    }

    // 9. Register GiveResources SimCallback on new state
    {
        lua_State* sL = sim_lua_state->raw();
        lua_pushstring(sL, "SimCallbacks");
        lua_rawget(sL, LUA_GLOBALSINDEX);
        if (!lua_istable(sL, -1)) {
            lua_pop(sL, 1);
            lua_newtable(sL);
            lua_pushstring(sL, "SimCallbacks");
            lua_pushvalue(sL, -2);
            lua_rawset(sL, LUA_GLOBALSINDEX);
        }
        sim_lua_state->do_string(R"(
            local sc = rawget(_G, 'SimCallbacks')
            sc.GiveResources = function(args)
                if not args or not args.From or not args.To then return end
                LOG('GiveResources: army ' .. tostring(args.From) .. ' -> army ' .. tostring(args.To) ..
                    ' mass=' .. tostring(args.Mass or 0) .. ' energy=' .. tostring(args.Energy or 0))
            end
        )");
        lua_settop(sL, 0);
    }

    // 10. Store game state manager in new sim registry
    {
        lua_State* sL = sim_lua_state->raw();
        lua_pushstring(sL, "__osc_game_state_mgr");
        lua_pushlightuserdata(sL, &game_state_mgr);
        lua_rawset(sL, LUA_REGISTRYINDEX);
    }

    // 11. Boot sim (registers moho/sim bindings, runs simInit.lua)
    osc::lua::SimLoader new_sim_loader;
    auto new_sim_result = new_sim_loader.boot_sim(
        *sim_lua_state, vfs, *sim_state);
    if (!new_sim_result) {
        spdlog::error("Reload sim boot failed: {}",
                      new_sim_result.error().message);
        return false;
    }

    // 12. Start the session from the setup
    {
        if (!replay && setup.slots.empty() && setup.ai_armies.empty() &&
            sim_state->army_count() >= 2) {
            // No sessionConfig: ARMY_2 is the AI (legacy behavior).
            setup.ai_armies = {1};
            spdlog::info("Session: fallback — ARMY_2 as AI (no sessionConfig)");
        } else if (!setup.ai_armies.empty()) {
            spdlog::info("Session: {} AI armies (personality={}), {} total slots",
                         setup.ai_armies.size(), setup.ai_personality, setup.army_count);
        }
        for (size_t i = 0; i < setup.slots.size(); ++i) {
            if (!setup.slots[i].configured) continue;
            if (auto* brain = sim_state->get_army(static_cast<osc::i32>(i)))
                brain->set_faction(setup.slots[i].faction);
        }
        osc::lua::SessionManager new_session_mgr;
        new_session_mgr.configure(setup);
        auto sess_result = new_session_mgr.start_session(
            *sim_lua_state, vfs, *sim_state, scenario_meta);
        if (!sess_result) {
            spdlog::warn("Reload session start failed: {}",
                         sess_result.error().message);
        }
        sim_state->set_game_setup(setup);
        // Every game records (for LastGame and --record); a replay plays.
        if (!replay) sim_state->set_recording(true);
    }

    // 13. Update UI state's sim_state registry pointer to new SimState
    {
        lua_pushstring(uiL, "osc_sim_state");
        lua_pushlightuserdata(uiL, sim_state.get());
        lua_rawset(uiL, LUA_REGISTRYINDEX);
    }

    // 14. Rebuild renderer scene
    if (renderer)
        renderer->build_scene(sim_state->terrain(), sim_state->blueprint_store(),
                              osc::sim::world_blueprints(*sim_state), &vfs, uiL);

    // 15. Reset camera to map center (spherical coords: target + distance)
    if (renderer && sim_state->terrain()) {
        osc::f32 cx = sim_state->terrain()->map_width() * 0.5f;
        osc::f32 cz = sim_state->terrain()->map_height() * 0.5f;
        renderer->camera().set_target(cx, cz);
        renderer->camera().set_distance(300.0f);
    }

    // 16. Update UI state registry pointers
    lua_pushstring(uiL, "__osc_scenario_path");
    lua_pushstring(uiL, launch_scenario.c_str());
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    lua_pushstring(uiL, "__osc_hover_entity_id");
    lua_pushnumber(uiL, 0);
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    lua_pushstring(uiL, "__osc_focus_army");
    lua_pushnumber(uiL, 0);
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    // 17. Clear selection
    if (input_handler) input_handler->set_selected({});
    if (prev_selection) prev_selection->clear();

    // 18. Reset game state
    game_state_mgr.set_game_over(false);
    game_state_mgr.set_paused(false, uiL);
    sim_accumulator = 0.0;

    // 19. Transition to GAME (skip if caller already handles transitions)
    if (game_state_mgr.current() != osc::GameState::LOADING) {
        // Headless/smoke-test path: do full transition
        game_state_mgr.transition_to(osc::GameState::LOADING, uiL);
    }
    game_state_mgr.transition_to(osc::GameState::GAME, nullptr); // nullptr = skip SetupUI

    spdlog::info("=== Map reload complete ===");
    return true;
}

/// Hand the UI's queued SimCallbacks to the sim. Moho runs them as
/// commands, inside a tick: in multiplayer they are broadcast and every peer
/// runs them on the same tick; in single-player they run at the next tick.
void submit_sim_callbacks(osc::sim::SimCallbackQueue& queue, osc::sim::SimState& sim) {
    for (auto& cb : queue.drain()) sim.submit_callback(std::move(cb));
}

/// Whether the cursor is over FA's UI rather than the world: the deepest
/// hit-testable control under it is neither a WorldView (FA's world input
/// surface) nor the root frame.
static bool mouse_over_ui(lua_State* uiL, osc::f64 x, osc::f64 y) {
    lua_pushstring(uiL, "__osc_root_frame");
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    osc::ui::UIControl* root = nullptr;
    if (lua_istable(uiL, -1)) {
        lua_pushstring(uiL, "_c_object");
        lua_rawget(uiL, -2);
        root = static_cast<osc::ui::UIControl*>(lua_touserdata(uiL, -1));
        lua_pop(uiL, 1);
    }
    lua_pop(uiL, 1);
    if (!root) return false;
    osc::ui::UIDispatch dispatch;
    auto* hit = dispatch.hit_test(uiL, root, x, y);
    return hit && hit != root && !dynamic_cast<osc::ui::WorldView*>(hit);
}

// ── FA's command mode (/lua/ui/game/commandmode.lua) ──
static constexpr const char* kCommandModeModule = "/lua/ui/game/commandmode.lua";

/// A unit blueprint's footprint from the UI state's blueprint store.
static void blueprint_footprint(lua_State* uiL, const std::string& bp_id, osc::f32& sx,
                                osc::f32& sz) {
    auto* store = osc::lua::LuaState::get_blueprint_store(uiL);
    auto* entry = store ? store->find(bp_id) : nullptr;
    if (!entry) return;
    store->push_lua_table(*entry, uiL);
    lua_pushstring(uiL, "Footprint");
    lua_rawget(uiL, -2);
    if (lua_istable(uiL, -1)) {
        lua_pushstring(uiL, "SizeX");
        lua_rawget(uiL, -2);
        if (lua_isnumber(uiL, -1)) sx = static_cast<osc::f32>(lua_tonumber(uiL, -1));
        lua_pop(uiL, 1);
        lua_pushstring(uiL, "SizeZ");
        lua_rawget(uiL, -2);
        if (lua_isnumber(uiL, -1)) sz = static_cast<osc::f32>(lua_tonumber(uiL, -1));
        lua_pop(uiL, 1);
    }
    lua_pop(uiL, 2); // Footprint + blueprint
}

/// FA's current command mode: GetCommandMode() -> {mode, data}, once the
/// game UI has loaded the module. Build mode carries the footprint (for
/// the ghost and the snap).
osc::renderer::CommandMode read_command_mode(lua_State* uiL) {
    osc::renderer::CommandMode m;
    osc::core::push_loaded_module_function(uiL, kCommandModeModule, "GetCommandMode");
    if (!lua_isfunction(uiL, -1)) {
        lua_pop(uiL, 1);
        return m;
    }
    if (lua_pcall(uiL, 0, 1, 0) != 0) {
        spdlog::warn("GetCommandMode error: {}", lua_tostring(uiL, -1));
        lua_pop(uiL, 1);
        return m;
    }
    if (lua_istable(uiL, -1)) {
        lua_rawgeti(uiL, -1, 1);
        if (lua_type(uiL, -1) == LUA_TSTRING) m.mode = lua_tostring(uiL, -1);
        lua_pop(uiL, 1);
        lua_rawgeti(uiL, -1, 2);
        if (lua_istable(uiL, -1)) {
            lua_pushstring(uiL, "name");
            lua_rawget(uiL, -2);
            if (lua_type(uiL, -1) == LUA_TSTRING) m.name = lua_tostring(uiL, -1);
            lua_pop(uiL, 1);
        }
        lua_pop(uiL, 1);
    }
    lua_pop(uiL, 1);
    if (m.mode == "build" && !m.name.empty())
        blueprint_footprint(uiL, m.name, m.footprint_x, m.footprint_z);
    return m;
}

/// Report an issued command to commandmode.OnCommandIssued, as Moho does:
/// a non-Shift command ends the mode, and FA draws its feedback blip.
void report_command_issued(lua_State* uiL, const osc::renderer::IssuedCommand& c) {
    lua_newtable(uiL);
    lua_pushstring(uiL, "CommandType");
    lua_pushstring(uiL, c.type.c_str());
    lua_rawset(uiL, -3);
    lua_pushstring(uiL, "Clear");
    lua_pushboolean(uiL, c.clear ? 1 : 0);
    lua_rawset(uiL, -3);
    if (!c.blueprint.empty()) {
        lua_pushstring(uiL, "Blueprint");
        lua_pushstring(uiL, c.blueprint.c_str());
        lua_rawset(uiL, -3);
    }
    lua_pushstring(uiL, "Target");
    lua_newtable(uiL);
    lua_pushstring(uiL, "Type");
    lua_pushstring(uiL, c.target_id ? "Entity" : "Position");
    lua_rawset(uiL, -3);
    if (c.target_id) {
        lua_pushstring(uiL, "EntityId");
        lua_pushnumber(uiL, c.target_id);
        lua_rawset(uiL, -3);
    }
    lua_pushstring(uiL, "Position");
    lua_newtable(uiL);
    lua_pushnumber(uiL, c.position.x);
    lua_rawseti(uiL, -2, 1);
    lua_pushnumber(uiL, c.position.y);
    lua_rawseti(uiL, -2, 2);
    lua_pushnumber(uiL, c.position.z);
    lua_rawseti(uiL, -2, 3);
    lua_rawset(uiL, -3); // Target.Position
    lua_rawset(uiL, -3); // command.Target
    osc::core::call_ui_callback(uiL, kCommandModeModule, "OnCommandIssued", 1);
}

/// Leave FA's command mode, cancelled (a right-click).
static void cancel_command_mode(lua_State* uiL) {
    lua_pushboolean(uiL, 1);
    osc::core::call_ui_callback(uiL, kCommandModeModule, "EndCommandMode", 1);
}

/// Show the build ghost while FA is in build mode. The ghost is cleared
/// only if this set it (other code may place ghosts too).
static void sync_build_ghost(osc::sim::SimState& sim, const osc::renderer::CommandMode& m,
                             bool& ghost_from_mode) {
    if (m.mode == "build" && !m.name.empty()) {
        sim.set_build_ghost(m.name, m.footprint_x, m.footprint_z);
        ghost_from_mode = true;
    } else if (ghost_from_mode) {
        sim.clear_build_ghost();
        ghost_from_mode = false;
    }
}

/// A selection action reaches the UI as Moho reports it,
/// gamemain.OnSelectionChanged(old, new, added, removed), and then the
/// engine's own AddOnSelectionChangedCallback callbacks. Moho reports every
/// action (`action`), even one that leaves the selection unchanged -- retail
/// refreshes the orders and construction panels on those -- as well as any
/// change. `prev` becomes `cur`.
void dispatch_selection_change(lua_State* uL, std::unordered_set<osc::u32>& prev,
                               const std::unordered_set<osc::u32>& cur, bool action) {
    if (cur == prev && !action) return;
    std::vector<osc::u32> old_ids(prev.begin(), prev.end());
    std::vector<osc::u32> new_ids(cur.begin(), cur.end());
    std::sort(old_ids.begin(), old_ids.end());
    std::sort(new_ids.begin(), new_ids.end());
    std::vector<osc::u32> added, removed;
    std::set_difference(new_ids.begin(), new_ids.end(), old_ids.begin(),
                        old_ids.end(), std::back_inserter(added));
    std::set_difference(old_ids.begin(), old_ids.end(), new_ids.begin(),
                        new_ids.end(), std::back_inserter(removed));
    prev = cur;

    osc::lua::push_units_for_ui(uL, old_ids);
    osc::lua::push_units_for_ui(uL, new_ids);
    osc::lua::push_units_for_ui(uL, added);
    osc::lua::push_units_for_ui(uL, removed);
    osc::core::call_ui_callback(uL, osc::core::kGameMainModule,
                                "OnSelectionChanged", 4);

    lua_pushstring(uL, "__osc_sel_changed_cbs");
    lua_rawget(uL, LUA_REGISTRYINDEX);
    if (lua_istable(uL, -1)) {
        const int cbs_idx = lua_gettop(uL);
        osc::lua::push_units_for_ui(uL, new_ids);
        const int arr_idx = lua_gettop(uL);
        const int n = luaL_getn(uL, cbs_idx); // Lua 5.0: no lua_objlen
        for (int ci = 1; ci <= n; ci++) {
            lua_rawgeti(uL, cbs_idx, ci);
            if (!lua_isfunction(uL, -1)) {
                lua_pop(uL, 1);
                continue;
            }
            lua_pushvalue(uL, arr_idx);
            if (lua_pcall(uL, 1, 0, 0) != 0) {
                const char* err = lua_tostring(uL, -1);
                spdlog::warn("OnSelectionChanged[{}] error: {}", ci,
                             err ? err : "(unknown)");
                lua_pop(uL, 1);
            }
        }
        lua_pop(uL, 1); // selection array
    }
    lua_pop(uL, 1); // callbacks table (or nil)
}

/// Moho's world-UI start (see ui::WldUIProvider): the user side of the
/// sync channel (/lua/UserSync.lua and its hooks: OnSync, a fresh Sync and
/// UnitData) is loaded for the new session, then uimain.StartGameUI makes the
/// Lua provider, whose loading dialog shows while the world loads.
void begin_world_ui(lua_State* uiL, osc::ui::WldUIProvider& wld) {
    if (auto r = osc::lua::run_vfs_script(uiL, "/lua/UserSync.lua"); !r)
        spdlog::warn("UserSync.lua: {}", r.error().message);
    osc::core::call_start_game_ui(uiL);
    wld.start_loading_dialog(uiL);
}

/// One Moho sim beat on the user side: the sync channel (sim -> UI data and
/// focus changes, OnSync), then the game UI's beat functions.
void world_beat(osc::lua::LuaState* sim_lua, osc::sim::SimState* sim, lua_State* uiL) {
    if (sim_lua) osc::lua::sync_beat(sim_lua->raw(), uiL);
    if (sim) osc::lua::notify_focus_army_damage(uiL, *sim);
    osc::core::call_game_beat(uiL);
}

/// Game over, as Moho reports it: once the sim has ended the session
/// (EndGame from the scenario's victory script, or the engine's own
/// adjudication without one), the UI hears uimain.NoteGameOver. Retail's
/// game-result UI (Sync.GameResult -> DoGameResult) announces the outcome and
/// offers the score screen: nothing is paused and the view is not switched.
void note_game_over_if_ended(osc::sim::SimState* sim, osc::GameStateManager& mgr, lua_State* uiL) {
    if (!sim || !sim->game_ended() || mgr.game_over()) return;
    mgr.set_game_over(true);
    const osc::i32 result = sim->player_result();
    spdlog::info("Game over: {}", result == 1   ? "VICTORY"
                                  : result == 2 ? "DEFEAT"
                                  : result == 3 ? "DRAW"
                                                : "ended");
    osc::core::call_note_game_over(uiL);
}

/// The world is loaded: build FA's game interface (gamemain.CreateUI) and
/// fade the loading dialog out.
void finish_world_ui(lua_State* uiL, osc::ui::WldUIProvider& wld, bool is_replay) {
    wld.create_game_interface(uiL, is_replay);
    wld.stop_loading_dialog(uiL);
}

/// Pump N UI frames: resume coroutines, fire OnBeat, fire beat functions.
void pump_ui_frames(osc::lua::LuaState& ui_lua_state, osc::sim::ThreadManager& ui_thread_manager,
                    osc::lua::BeatFunctionRegistry& beat_registry, int count,
                    osc::u32& ui_frame_counter) {
    lua_State* uL = ui_lua_state.raw();
    for (int i = 0; i < count; i++) {
        ui_frame_counter++;
        osc::lua::advance_ui_clock(uL, 1.0 / 60.0);
        ui_thread_manager.resume_all(ui_frame_counter);
        osc::core::call_on_beat(uL, 1.0 / 30.0);
        beat_registry.fire_all(uL);
    }
}

void pump_ui_frames_with_controls(osc::lua::LuaState& ui_lua_state,
                                  osc::sim::ThreadManager& ui_thread_manager,
                                  osc::lua::BeatFunctionRegistry& beat_registry,
                                  osc::ui::UIControlRegistry& ui_registry, int count,
                                  osc::u32& ui_frame_counter) {
    lua_State* uL = ui_lua_state.raw();
    osc::ui::UIDispatch dispatch;
    for (int i = 0; i < count; i++) {
        ui_frame_counter++;
        osc::lua::advance_ui_clock(uL, 1.0 / 60.0);
        ui_thread_manager.resume_all(ui_frame_counter);
        dispatch.update_controls(uL, ui_registry, 1.0 / 60.0);
        dispatch.dispatch_events(uL, ui_registry);
        osc::core::call_on_beat(uL, 1.0 / 30.0);
        beat_registry.fire_all(uL);
    }
}

// Build a fixed 1v1 human-vs-human sessionConfig for `scenario` and launch it via
// the existing LaunchSinglePlayerSession global. Both LAN peers build the same
// config (they differ only in which army is locally focused, decided by role).
static void lan_launch_session(lua_State* uL, const std::string& scenario) {
    lua_pushstring(uL, "LaunchSinglePlayerSession");
    lua_rawget(uL, LUA_GLOBALSINDEX);
    if (!lua_isfunction(uL, -1)) {
        lua_pop(uL, 1);
        spdlog::warn("[lan] LaunchSinglePlayerSession not available");
        return;
    }
    lua_newtable(uL); // config
    lua_pushstring(uL, "ScenarioFile");
    lua_pushstring(uL, scenario.c_str());
    lua_rawset(uL, -3);
    lua_pushstring(uL, "GameOptions");
    lua_newtable(uL);
    lua_pushstring(uL, "ScenarioFile");
    lua_pushstring(uL, scenario.c_str());
    lua_rawset(uL, -3);
    lua_rawset(uL, -3);
    lua_pushstring(uL, "PlayerOptions");
    lua_newtable(uL);
    auto push_slot = [&](int idx, const char* name, int faction, int team) {
        lua_pushnumber(uL, idx);
        lua_newtable(uL);
        lua_pushstring(uL, "Human"); lua_pushboolean(uL, 1); lua_rawset(uL, -3);
        lua_pushstring(uL, "PlayerName"); lua_pushstring(uL, name); lua_rawset(uL, -3);
        lua_pushstring(uL, "Faction"); lua_pushnumber(uL, faction); lua_rawset(uL, -3);
        lua_pushstring(uL, "Team"); lua_pushnumber(uL, team); lua_rawset(uL, -3);
        lua_pushstring(uL, "StartSpot"); lua_pushnumber(uL, idx); lua_rawset(uL, -3);
        lua_rawset(uL, -3);
    };
    push_slot(1, "Host", 1, 1);
    push_slot(2, "Client", 2, 2);
    lua_rawset(uL, -3); // config.PlayerOptions
    if (lua_pcall(uL, 1, 0, 0) != 0) {
        spdlog::warn("[lan] LaunchSinglePlayerSession error: {}",
                     lua_tostring(uL, -1) ? lua_tostring(uL, -1) : "(unknown)");
        lua_pop(uL, 1);
    }
}

/// A test mode's flag given to the game (the integration runner has them),
/// or null.
static const char* test_mode_flag(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool test = arg.size() > 7 && arg.starts_with("--") && arg.ends_with("-test") &&
                          arg != "--replay-flow-test";
        if (test || arg == "--render-dump" || arg == "--mp-host" || arg == "--mp-join" ||
            arg == "--lan-host" || arg == "--lan-join")
            return argv[i];
    }
    return nullptr;
}

int run(int argc, char* argv[], TestModes* tests) {
    osc::log::init();
    osc::platform::install_crash_handler();

    auto config = parse_args(argc, argv, tests);

    // The test modes (the integration runner's): which the command line
    // asks for, and those that need no engine.
    const TestRequest request = tests ? tests->parse(argc, argv) : TestRequest{};
    if (tests) {
        if (auto code = tests->before_boot(argc, argv)) return *code;
    } else if (const char* flag = test_mode_flag(argc, argv)) {
        spdlog::error("{} is a test mode: run it with osc_integration", flag);
        return 2;
    }

    auto map_path = parse_map_arg(argc, argv);
    auto tick_count = parse_ticks_arg(argc, argv);
    // --replay <file>: play a recorded game (its setup names the map).
    std::optional<osc::sim::Replay> replay_to_play;
    if (const auto replay_arg = parse_string_arg(argc, argv, "--replay", ""); !replay_arg.empty()) {
        replay_to_play = load_replay(replay_arg);
        if (!replay_to_play) return 1;
        map_path = replay_to_play->setup.scenario;
    }
    g_record_path = parse_string_arg(argc, argv, "--record", "");
    const bool scripted_orders = parse_flag(argc, argv, "--scripted-orders");
    // Scripted runs of the windowed loop: offscreen, silent, fixed clock.
    // --watch <file>: open a replay in the game, as the replay dialog does.
    // --replay-flow-test: the dialog's own path (the first replay
    // GetSpecialFiles lists), played to its end offscreen.
    const std::string watch_path = parse_string_arg(argc, argv, "--watch", "");
    const bool replay_flow_test = parse_flag(argc, argv, "--replay-flow-test");
    const bool scripted_window = request.windowed || replay_flow_test;
    bool no_fog = parse_flag(argc, argv, "--no-fog");
    bool legacy_hud = parse_flag(argc, argv, "--legacy-hud");
    bool no_decals = parse_flag(argc, argv, "--no-decals");
    bool profile_enabled = parse_flag(argc, argv, "--profile");
    bool ai_skirmish = parse_flag(argc, argv, "--ai-skirmish");
    bool instrument = parse_flag(argc, argv, "--instrument");
    bool builder_debug = parse_flag(argc, argv, "--builder-debug");
    auto ai_personality = parse_string_arg(argc, argv, "--ai-personality", "adaptive");
    // --ai-armies <n>: how many of the scenario's armies play (all AI).
    size_t ai_army_count = 2;
    if (const auto n = parse_string_arg(argc, argv, "--ai-armies", ""); !n.empty()) {
        ai_army_count = static_cast<size_t>(std::max(1, std::atoi(n.c_str())));
    }

    // Collect all command-line args for HasCommandLineArg (M147d)
    std::set<std::string> cmdline_args;
    for (int i = 1; i < argc; ++i) {
        cmdline_args.insert(argv[i]);
    }

    // A checked run: headless, and its exit code is the checks' result (a
    // test mode, or an AI game whose script errors count).
    const bool any_test = request.headless || ai_skirmish;
    bool headless = (tick_count > 0) || any_test || replay_to_play.has_value();
    // (A scripted windowed test mode counts its script errors if it says
    // so, in parse.)
    if (any_test || replay_flow_test) osc::test_status::set_count_lua_failures(true);

    if (config.fa_path.empty() || config.init_file.empty()) {
        spdlog::error("Supreme Commander: Forged Alliance not found. Pass "
                      "--fa-path <dir>, set OSC_FA_PATH, or run --print-install "
                      "to see where we looked.");
        const bool data_test_mode =
            any_test || !parse_string_arg(argc, argv, "--binding-coverage", "").empty();
        return data_test_mode ? kExitSkippedNoData : 1;
    }

    spdlog::info("FA path:   {}", config.fa_path.string());
    spdlog::info("Init file: {}", config.init_file.string());
    spdlog::info("FAF data:  {}", config.faf_data_path.string());

    if (tests) {
        if (auto code = tests->before_init(config)) return *code;
    }

    if (!osc::fs::exists(config.init_file)) {
        spdlog::error("Init file not found: {}", config.init_file.string());
        return 1;
    }

    if (builder_debug) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::info("Builder debug mode enabled — verbose logging active");
    }

    // Phase 1: Init + VFS (sim Lua state)
    auto sim_lua_state = std::make_unique<osc::lua::LuaState>();
    osc::vfs::VirtualFileSystem vfs;
    osc::lua::InitLoader loader;

    auto init_result = loader.execute_init(*sim_lua_state, config, vfs);
    if (!init_result) {
        spdlog::error("Init failed: {}", init_result.error().message);
        return 1;
    }

    // Phase 2: Blueprint loading (sim Lua state owns the store)
    osc::blueprints::BlueprintStore store(sim_lua_state->raw());

    auto bp_result = loader.load_blueprints(*sim_lua_state, vfs, store);
    if (!bp_result) {
        spdlog::error("Blueprint loading failed: {}",
                       bp_result.error().message);
        return 1;
    }

    // Spot check
    auto* acu = store.find("uel0001");
    if (acu) {
        auto desc = store.get_string_field(*acu, "Description");
        spdlog::info("Spot check: uel0001 found ({})",
                     desc.value_or("no description"));
    } else {
        spdlog::warn("Spot check: uel0001 (UEF ACU) not found");
    }

    spdlog::info("OpenSupCom initialization complete.");

    // Audio: FA's sounds for the whole run (front end, lobby, games). Tests
    // and captures run it without an output device; headless runs have no
    // frames, so their sim tick is its clock.
    const bool silent_capture = !parse_string_arg(argc, argv, "--screenshot", "").empty() ||
                                !parse_string_arg(argc, argv, "--golden", "").empty() || scripted_window;
    osc::audio::SoundManager sound(config.fa_path / "sounds", !headless && !silent_capture);
    // --seed N: the game's random seed (weapon spread, scripts' Random and
    // math.random). Runs that must repeat default to a fixed one.
    const std::string seed_arg = parse_string_arg(argc, argv, "--seed", "");
    const bool reproducible_run = headless || scripted_window || silent_capture;
    std::ofstream checksum_trace;
    if (const auto trace = parse_string_arg(argc, argv, "--checksum-trace", ""); !trace.empty()) {
        checksum_trace.open(trace, std::ios::trunc);
        if (!checksum_trace) {
            spdlog::error("--checksum-trace: cannot write {}", trace);
            return 1;
        }
        g_checksum_trace = &checksum_trace;
    }
    std::ofstream entity_trace;
    if (const auto trace = parse_string_arg(argc, argv, "--entity-trace", ""); !trace.empty()) {
        entity_trace.open(trace, std::ios::trunc);
        if (!entity_trace) {
            spdlog::error("--entity-trace: cannot write {}", trace);
            return 1;
        }
        g_entity_trace = &entity_trace;
        const auto range = parse_string_arg(argc, argv, "--entity-trace-ticks", "");
        if (const auto dash = range.find('-'); dash != std::string::npos) {
            g_entity_trace_from = static_cast<osc::u32>(std::stoul(range.substr(0, dash)));
            g_entity_trace_to = static_cast<osc::u32>(std::stoul(range.substr(dash + 1)));
        }
    }
    std::ofstream rng_trace;
    if (const auto trace = parse_string_arg(argc, argv, "--rng-trace", ""); !trace.empty()) {
        rng_trace.open(trace, std::ios::trunc);
        if (!rng_trace) {
            spdlog::error("--rng-trace: cannot write {}", trace);
            return 1;
        }
        g_rng_trace = &rng_trace;
        const auto range = parse_string_arg(argc, argv, "--rng-trace-ticks", "");
        if (const auto dash = range.find('-'); dash != std::string::npos) {
            g_rng_trace_from = static_cast<osc::u32>(std::stoul(range.substr(0, dash)));
            g_rng_trace_to = static_cast<osc::u32>(std::stoul(range.substr(dash + 1)));
        }
    }
    sound.set_sim_clocked(headless);

    // Phase 3: Map + Sim boot (only when --map provided)
    WorldInterp world_interp; // outlives every sim it observes
    std::unique_ptr<osc::sim::SimState> sim_state;
    osc::lua::ScenarioMetadata scenario_meta;

    // The game's setup: a replay's own, or the command line's.
    osc::sim::GameSetup game_setup;
    if (replay_to_play) {
        game_setup = replay_to_play->setup;
    } else {
        game_setup.scenario = map_path;
        game_setup.seed = new_game_seed(seed_arg, reproducible_run);
        if (ai_skirmish) {
            // Every army the AI's (--ai-armies of them); listed once they exist.
            game_setup.army_count = static_cast<int>(ai_army_count);
            game_setup.ai_personality = ai_personality;
            // Cheat variants: a personality ending in "cheat"
            if (ai_personality.size() > 5 &&
                ai_personality.compare(ai_personality.size() - 5, 5, "cheat") == 0) {
                game_setup.cheat_mult = 2.0;
                game_setup.build_mult = 2.0;
            }
        } else if (request.ai_army_2) {
            game_setup.ai_armies = {1}; // ARMY_2 (0-based index 1) is AI
        }
    }
    // --record: the last game's replay is written as the run ends, however
    // it ends (the normal end writes it before logging shuts down).
    struct RecordingWriter {
        std::unique_ptr<osc::sim::SimState>& sim;
        bool written = false;
        void write() {
            if (written) return;
            written = true;
            if (!g_record_path.empty() && sim && sim->recording())
                write_recording(*sim, g_record_path);
        }
        ~RecordingWriter() { write(); }
    } recording_writer{sim_state};

    if (!map_path.empty()) {
    sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
    sim_state->set_seed(game_setup.seed);
    sim_state->set_checksum_trace(g_checksum_trace);
    sim_state->set_entity_trace(g_entity_trace, g_entity_trace_from, g_entity_trace_to);
    sim_state->set_rng_trace(g_rng_trace, g_rng_trace_from, g_rng_trace_to);
    spdlog::info("Game seed {:#018x}", game_setup.seed);

    attach_sound(*sim_lua_state, *sim_state, &sound);
    // Only a drawn world needs its ticks captured.
    if (!headless) world_interp.attach(*sim_state);

    // Bone cache (lazy-loaded per-blueprint SCM bone data)
    auto bone_cache = std::make_unique<osc::sim::BoneCache>(&vfs, &store);
    sim_state->set_bone_cache(std::move(bone_cache));

    // Animation cache (lazy-loaded SCA animation data)
    auto anim_cache = std::make_unique<osc::sim::AnimCache>(&vfs);
    sim_state->set_anim_cache(std::move(anim_cache));

    // Load scenario and map
    {
        osc::lua::ScenarioLoader scenario_loader;
        auto meta_result = scenario_loader.load_scenario(
            *sim_lua_state, vfs, map_path, *sim_state);
        if (!meta_result) {
            spdlog::error("Scenario load failed: {}",
                          meta_result.error().message);
            return 1;
        }
        scenario_meta = meta_result.value();

        // The setup's armies (all of the scenario's when it doesn't say)
        const size_t army_limit = game_setup.army_count > 0
                                      ? static_cast<size_t>(game_setup.army_count)
                                      : scenario_meta.armies.size();
        for (size_t i = 0; i < std::min(army_limit, scenario_meta.armies.size()); i++) {
            sim_state->add_army(scenario_meta.armies[i], scenario_meta.armies[i]);
        }
        if (ai_skirmish && !replay_to_play) {
            for (size_t a = 0; a < sim_state->army_count(); ++a)
                game_setup.ai_armies.push_back(static_cast<int>(a));
        }
    }

    // Fallback: add a default army if none from scenario
    if (sim_state->army_count() == 0) {
        sim_state->add_army("ARMY_1", "Player");
    }

    osc::lua::SimLoader sim_loader;
    auto sim_result = sim_loader.boot_sim(*sim_lua_state, vfs, *sim_state);
    if (!sim_result) {
        spdlog::error("Sim boot failed: {}", sim_result.error().message);
        return 1;
    }

    // Register GiveResources SimCallback handler (M151b)
    // Use rawget/rawset to bypass config.lua global lock
    {
        lua_State* sL = sim_lua_state->raw();
        // Get or create SimCallbacks table
        lua_pushstring(sL, "SimCallbacks");
        lua_rawget(sL, LUA_GLOBALSINDEX);
        if (!lua_istable(sL, -1)) {
            lua_pop(sL, 1);
            lua_newtable(sL);
            lua_pushstring(sL, "SimCallbacks");
            lua_pushvalue(sL, -2);
            lua_rawset(sL, LUA_GLOBALSINDEX);
        }
        // Register GiveResources function
        auto give_res = sim_lua_state->do_string(R"(
            local sc = rawget(_G, 'SimCallbacks')
            sc.GiveResources = function(args)
                if not args or not args.From or not args.To then return end
                LOG('GiveResources: army ' .. tostring(args.From) .. ' -> army ' .. tostring(args.To) ..
                    ' mass=' .. tostring(args.Mass or 0) .. ' energy=' .. tostring(args.Energy or 0))
            end
        )");
        if (!give_res) {
            spdlog::warn("GiveResources SimCallback registration error: {}", give_res.error().message);
        }
        lua_settop(sL, 0); // clean stack
    }
    } // end if (!map_path.empty()) — Phase 3

    // === UI Lua State ===
    osc::lua::LuaState ui_lua_state;
    ui_lua_state.set_vfs(&vfs);
    ui_lua_state.set_blueprint_store(&store);
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "osc_sound_manager");
        lua_pushlightuserdata(uL, &sound);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Run init sequence on UI state (polyfills, config, class system, import)
    auto ui_init_result = loader.execute_init(ui_lua_state, config, vfs);
    if (!ui_init_result) {
        spdlog::error("UI Lua init failed: {}", ui_init_result.error().message);
        return 1;
    }

    // Load blueprints into UI state (separate store — UI refs must not
    // overwrite sim_L registry refs in the main store)
    osc::blueprints::BlueprintStore ui_store(ui_lua_state.raw());
    auto ui_bp_result = loader.load_blueprints(ui_lua_state, vfs, ui_store);
    if (!ui_bp_result) {
        spdlog::warn("UI blueprint load: {}", ui_bp_result.error().message);
    }

    // Register moho class tables on UI state (unit_methods, etc.)
    // When no map, create a temporary dummy SimState for registration.
    // Sim-dependent moho methods check get_sim(L) and return gracefully when null.
    {
        std::unique_ptr<osc::sim::SimState> dummy_sim;
        if (!sim_state) {
            dummy_sim = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
        }
        osc::lua::register_moho_bindings(ui_lua_state, sim_state ? *sim_state : *dummy_sim);
        // A drawn game's captured ticks serve the UI's unit objects too.
        if (!headless) osc::lua::set_ui_world_source(ui_lua_state.raw(), &world_interp.history);
        // If we used a dummy, clear the sim pointer in UI registry so moho methods
        // return gracefully instead of dereferencing a dangling pointer.
        if (dummy_sim) {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "osc_sim_state");
            lua_pushlightuserdata(uL, nullptr);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }
    }

    // Localization cache — load strings from VFS, then store pointer in UI registry
    osc::core::Localization loc_cache;
    loc_cache.load_from_vfs(ui_lua_state.raw(), &vfs);
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_loc_cache");
        lua_pushlightuserdata(uL, &loc_cache);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Preferences (Game.prefs). An interactive game keeps them in the user's
    // config dir. Tests and captures never touch that file, so the player's
    // settings cannot change a result: --prefs PATH seeds them instead, and
    // is written back only when interactive.
    const bool interactive = !headless &&
                             parse_string_arg(argc, argv, "--screenshot", "").empty() &&
                             parse_string_arg(argc, argv, "--golden", "").empty();
    osc::core::Preferences prefs;
    {
        std::filesystem::path file = parse_string_arg(argc, argv, "--prefs", "");
        if (file.empty() && interactive) {
            file = osc::platform::known_folder(osc::platform::KnownFolder::Config) /
                   "opensupcom" / "Game.prefs";
        }
        if (!file.empty()) prefs.load(file);
        if (interactive) prefs.set_path(file);
        // Retail's menus need a current profile; it would ask for one in a
        // first-run dialog (and then offer the tutorial). The engine makes
        // "Player" instead.
        if (prefs.ensure_profile("Player")) {
            prefs.set_bool(prefs.current_profile_path() + ".MenuTutorialPrompt", true);
            prefs.save();
        }
    }
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_preferences");
        lua_pushlightuserdata(uL, &prefs);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Replays and saved games (FA's special files). An interactive game keeps
    // them in FA's user folder; other runs use --user-dir, else a temporary
    // folder of their own (removed at exit), never the player's.
    std::filesystem::path user_dir = parse_string_arg(argc, argv, "--user-dir", "");
    std::filesystem::path temp_user_dir;
    if (user_dir.empty() && interactive) user_dir = osc::lua::SpecialFiles::default_root();
    if (user_dir.empty()) {
        std::random_device rd;
        temp_user_dir =
            std::filesystem::temp_directory_path() / fmt::format("opensupcom-user-{:08x}", rd());
        user_dir = temp_user_dir;
    }
    struct TempDirRemover {
        std::filesystem::path dir;
        ~TempDirRemover() {
            std::error_code ec;
            if (!dir.empty()) std::filesystem::remove_all(dir, ec);
        }
    } temp_user_dir_remover{temp_user_dir};
    osc::lua::SpecialFiles special_files(user_dir);
    osc::lua::register_special_file_bindings(ui_lua_state, &special_files);
    // FA's LastGame: the game just left, as recorded, in the current
    // profile's replays -- when a new game starts, on the way back to the
    // lobby, and at exit. Interactive games only; a replay isn't recorded.
    auto save_last_game = [&]() {
        if (!interactive || !sim_state || !sim_state->recording() || sim_state->playback()) return;
        const auto& replay = sim_state->recorded_replay();
        if (!replay.has_setup || replay.final_tick == 0) return;
        const std::string profile =
            prefs.get_string(prefs.current_profile_path() + ".Name", "Player");
        const auto* type = osc::lua::SpecialFiles::find_type("Replay");
        const auto path = special_files.path(*type, profile, "LastGame");
        if (path.empty()) {
            spdlog::warn("Replay: profile name '{}' can't be a folder name; LastGame not saved",
                         profile);
            return;
        }
        osc::lua::write_replay_file(replay, path);
    };

    // WldUIProvider — long-lived instance stored in registry for InternalCreateWldUIProvider
    osc::ui::WldUIProvider wld_provider;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_wld_ui_provider");
        lua_pushlightuserdata(uL, &wld_provider);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // UI control registry (M71)
    osc::ui::UIControlRegistry ui_registry;

    // Register file I/O bindings on ui_L for lobby map enumeration (M148c)
    // IMPORTANT: must come BEFORE register_ui_bindings so that the real
    // ForkThread/WaitSeconds/Sound overwrite the blueprint stubs.
    osc::lua::register_blueprint_bindings(ui_lua_state);

    osc::lua::register_ui_bindings(ui_lua_state, ui_registry);
    osc::lua::register_user_bindings(ui_lua_state);

    // Set root frame size to window dimensions (1600x900) so LazyVar layout
    // resolves correctly. Must happen BEFORE CreateUI() so FillParent etc. work.
    {
        ui_lua_state.do_string(
            "local f = GetFrame(0)\n"
            "if f then\n"
            "  f.Width:Set(1600)\n"
            "  f.Height:Set(900)\n"
            "  f.Left:Set(0)\n"
            "  f.Top:Set(0)\n"
            "  f.Right:Set(1600)\n"
            "  f.Bottom:Set(900)\n"
            "end\n");
    }

    // UI-side thread manager (reuses ThreadManager with frame counts instead of sim ticks)
    osc::sim::ThreadManager ui_thread_manager(ui_lua_state.raw());
    ui_thread_manager.register_in_registry(ui_lua_state.raw());
    osc::u32 ui_frame_count = 0;

    // Also store under __osc_ui_thread_manager for ForkThread lookup.
    // register_in_registry stores under "osc_thread_mgr" for Destroy() support.
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_ui_thread_manager");
        lua_pushlightuserdata(uL, &ui_thread_manager);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // ── Engine state machine ──
    // Store game state string in UI registry (source of truth for GetCurrentUIState)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_game_state");
        lua_pushstring(uL, "game");
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Set focus army (0 = ARMY_1)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_focus_army");
        lua_pushnumber(uL, 0);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Register engine globals on UI Lua state
    // Note: GetCurrentUIState and WorldIsLoading are registered by register_ui_bindings (M144c)
    ui_lua_state.register_function("FlushEvents", l_FlushEvents);
    ui_lua_state.register_function("SessionIsReplay", l_SessionIsReplay);
    ui_lua_state.register_function("SessionGetScenarioInfo", l_SessionGetScenarioInfo);
    ui_lua_state.register_function("GetEconomyTotals", l_GetEconomyTotals);
    ui_lua_state.register_function("GetSimTicksPerSecond", l_GetSimTicksPerSecond);
    ui_lua_state.register_function("GetArmiesTable", l_GetArmiesTable);
    ui_lua_state.register_function("IsAlly", l_ui_IsAlly);

    // User-state init, as Moho runs it for every UI state (front end and
    // game): retail /lua/userInit.lua -- plus its /schook hook -- runs
    // globalInit (which turns the moho.* tables into Lua classes, so MAUI's
    // Class(moho.frame_methods, Control) works), and defines WaitSeconds,
    // FrontEndData and the Prefetcher. The engine globals it and the UI
    // bootstrap expect must exist first.
    osc::lua::register_prefetch_bindings(ui_lua_state);
    osc::lua::register_category_bindings(ui_lua_state);
    {
        lua_State* uL = ui_lua_state.raw();
        auto global_is_defined = [&](const char* name) {
            lua_pushstring(uL, name);
            lua_rawget(uL, LUA_GLOBALSINDEX);
            const bool defined = !lua_isnil(uL, -1);
            lua_pop(uL, 1);
            return defined;
        };
        auto set_stub = [&](const char* name) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushcfunction(uL, [](lua_State*) -> int { return 0; });
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        auto set_str = [&](const char* name, const char* val) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushstring(uL, val);
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        auto set_bool_fn = [&](const char* name, bool val) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushcfunction(uL, val ?
                +[](lua_State* L) -> int { lua_pushboolean(L, 1); return 1; } :
                +[](lua_State* L) -> int { lua_pushboolean(L, 0); return 1; });
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        set_stub("AudioSetLanguage");
        set_str("__language", "us");
        set_bool_fn("HasLocalizedVO", false);
        // Engine globals needed by FA's UI bootstrap chain
        // (GetOptions, GetVolume, SetVolume, etc.)
        auto set_nil_fn = [&](const char* name) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushcfunction(uL, [](lua_State* L) -> int {
                lua_pushnil(L);
                return 1;
            });
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        set_stub("ConExecute");            // console commands
        set_stub("ConExecuteSave");        // console commands
        set_stub("AddInputCapture");       // input system
        set_stub("RemoveInputCapture");    // input system
        set_bool_fn("AnyInputCapture", false);
        set_bool_fn("DebugFacilitiesEnabled", false);
        set_stub("ExitApplication");       // exit
        set_stub("PrefetchSession");       // loading optimization
        set_stub("SetFocusArmy");          // army focus
        set_nil_fn("GetFocusArmy");        // army focus
        set_stub("ClearFrame");            // UI cleanup
        set_stub("GpgNetSend");            // multiplayer
        set_bool_fn("HasCommandLineArg2", false); // command line
        // Session functions
        set_bool_fn("SessionIsActive", false);
        set_bool_fn("SessionIsMultiplayer", false);
        set_bool_fn("SessionIsObservingAllowed", false);
        set_bool_fn("SessionIsBeingRecorded", false);
        set_bool_fn("SessionCanRestart", false);
        set_nil_fn("SessionGetCommandSourceNames");
        set_nil_fn("SessionGetLocalCommandSource");
        // System info
        set_nil_fn("GetMouseScreenPos");
        set_stub("SetOverlayFilter");
        set_stub("SetOverlayFilters");
        set_nil_fn("GetActiveBuildTemplate");
        set_nil_fn("GetHighlightCommand");
        set_nil_fn("GetInputCapture");
        set_stub("RemoveInputCapture");
        set_stub("RestartSession");
        set_nil_fn("GetAntiAliasingOptions");
        set_nil_fn("GetResolution");
        set_stub("SetResolution");
        // __installedlanguages — table of available language codes
        if (!global_is_defined("__installedlanguages")) {
            lua_pushstring(uL, "__installedlanguages");
            lua_newtable(uL);
            lua_pushstring(uL, "us"); lua_rawseti(uL, -2, 1);
            lua_rawset(uL, LUA_GLOBALSINDEX);
        }
    }
    {
        const char* init_script = vfs.file_exists("/lua/userInit.lua")
                                      ? "/lua/userInit.lua" : "/lua/globalInit.lua";
        if (auto r = osc::lua::run_vfs_script(ui_lua_state.raw(), init_script)) {
            spdlog::info("Loaded {} on ui_L", init_script);
        } else {
            spdlog::warn("{} error: {}", init_script, r.error().message);
        }
    }

    // State transition: INIT → GAME or INIT → FRONT_END
    if (!map_path.empty()) {
        osc::core::call_setup_ui(ui_lua_state.raw());
    } else {
        // No map: bootstrap front-end menu UI
        // 2. Call SetupUI() (creates cursor, sets skin)
        osc::core::call_setup_ui(ui_lua_state.raw());
        // 3. Call import('/lua/ui/menus/main.lua').CreateUI()
        {
            auto r = ui_lua_state.do_string(
                "import('/lua/ui/menus/main.lua').CreateUI()");
            if (r) {
                spdlog::info("Front-end menu CreateUI() succeeded");
            } else {
                spdlog::warn("Front-end CreateUI error: {}", r.error().message);
            }
            // Add the LAN Game button + IP/Host/Join dialog to the front end.
            {
                auto lr = ui_lua_state.do_string(osc::lua::kLanDialogLua);
                if (!lr) spdlog::warn("LAN dialog UI error: {}", lr.error().message);
            }
        }
        // Auto-trigger Skirmish: bypass lobby UI, directly launch with sessionConfig
        if (parse_flag(argc, argv, "--auto-skirmish")) {
            ui_lua_state.do_string(R"(
                ForkThread(function()
                    WaitSeconds(2.0)
                    LOG('Auto-skirmish: launching with 1 human + 1 AI...')
                    LaunchSinglePlayerSession({
                        ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
                        GameOptions = {
                            ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
                        },
                        PlayerOptions = {
                            [1] = {
                                Human = true,
                                PlayerName = 'Player',
                                Faction = 1,
                                Team = 1,
                                StartSpot = 1,
                            },
                            [2] = {
                                Human = false,
                                PlayerName = 'AI: Adaptive',
                                AIPersonality = 'adaptive',
                                Faction = 2,
                                Team = 2,
                                StartSpot = 2,
                            },
                        },
                    })
                end)
            )");
        }
        // Auto-trigger lobby via ButtonSkirmish (tests full lobby flow)
        if (parse_flag(argc, argv, "--auto-lobby")) {
            ui_lua_state.do_string(R"(
                ForkThread(function()
                    WaitSeconds(2.0)
                    LOG('Auto-lobby: triggering ButtonSkirmish...')
                    import('/lua/ui/menus/main.lua').ButtonSkirmish()
                end)
            )");
        }
        spdlog::info("Front-end UI initialized (no --map)");
    }

    spdlog::info("Dual Lua states initialized (sim_L + ui_L)");

    // Terrain query test
    if (sim_state && sim_state->terrain()) {
        auto* t = sim_state->terrain();
        osc::f32 cx = static_cast<osc::f32>(t->map_width()) / 2;
        osc::f32 cz = static_cast<osc::f32>(t->map_height()) / 2;
        spdlog::info("Terrain test:");
        spdlog::info("  Center ({}, {}): terrain={:.1f}, surface={:.1f}",
                     cx, cz,
                     t->get_terrain_height(cx, cz),
                     t->get_surface_height(cx, cz));
    }

    // Phase 4: Session lifecycle
    if (!map_path.empty()) {
        osc::lua::SessionManager session_mgr;
        session_mgr.configure(game_setup);
        auto session_result = session_mgr.start_session(
            *sim_lua_state, vfs, *sim_state, scenario_meta);
        if (!session_result) {
            spdlog::error("Session start failed: {}",
                          session_result.error().message);
            return 1;
        }
        sim_state->set_game_setup(game_setup);
        // Recorded for --record, and an interactive game for its LastGame.
        if (!g_record_path.empty() || interactive) sim_state->set_recording(true);
        if (replay_to_play) return play_replay(*sim_state, *replay_to_play);
    }

    // Binding-coverage report (roadmap M184): runs on the fully booted sim and
    // UI states, then exits.
    if (const std::string coverage_out =
            parse_string_arg(argc, argv, "--binding-coverage", "");
        !coverage_out.empty()) {
        if (!sim_lua_state) {
            spdlog::error("--binding-coverage needs --map <scenario>");
            return 1;
        }
        return osc::lua::coverage::run_coverage_report(
            sim_lua_state->raw(), ui_lua_state.raw(), vfs, coverage_out,
            parse_string_arg(argc, argv, "--binding-baseline", ""));
    }

    // Enable profiler if requested
    if (profile_enabled) {
        osc::Profiler::instance().set_enabled(true);
        spdlog::info("Performance profiling enabled");
    }

    // BeatFunctionRegistry for per-frame Lua callbacks (M145b) — outer scope for headless test access
    osc::lua::BeatFunctionRegistry beat_registry;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_beat_registry");
        lua_pushlightuserdata(uL, &beat_registry);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Key map registry for hotkey dispatch (M150b) — outer scope for headless test access
    osc::ui::KeyMapRegistry keymap_registry;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_keymap_registry");
        lua_pushlightuserdata(uL, &keymap_registry);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // FrontEndData — cross-state key-value store (M147c)
    osc::FrontEndData front_end_data;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_front_end_data");
        lua_pushlightuserdata(uL, &front_end_data);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Command-line args for HasCommandLineArg (M147d)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_cmdline_args");
        lua_pushlightuserdata(uL, &cmdline_args);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // GameStateManager — outer scope for headless test access (M144b)
    osc::GameStateManager game_state_mgr;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_game_state_mgr");
        lua_pushlightuserdata(uL, &game_state_mgr);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }
    // SetupUI already ran during the UI state's boot above; the initial
    // transitions pass nullptr so it does not run a second time.
    if (!map_path.empty()) {
        game_state_mgr.transition_to(osc::GameState::GAME, nullptr);
        if (sim_lua_state) {
            lua_State* sL = sim_lua_state->raw();
            lua_pushstring(sL, "__osc_game_state_mgr");
            lua_pushlightuserdata(sL, &game_state_mgr);
            lua_rawset(sL, LUA_REGISTRYINDEX);
        }
        // FA's game interface. Headless test modes other than those asking
        // for it keep a bare root frame: they build and inspect their own
        // controls.
        if (!headless || request.world_ui) {
            begin_world_ui(ui_lua_state.raw(), wld_provider);
            finish_world_ui(ui_lua_state.raw(), wld_provider, false);
        }
    } else {
        // No map: start in FRONT_END state (main menu)
        game_state_mgr.transition_to(osc::GameState::FRONT_END, nullptr);
    }

    // What a test mode drives.
    Engine engine{config,         map_path,
                  tick_count,     seed_arg,
                  ai_personality, vfs,
                  store,          loader,
                  sim_lua_state,  sim_state,
                  scenario_meta,  ui_lua_state,
                  ui_registry,    ui_thread_manager,
                  ui_frame_count, beat_registry,
                  game_state_mgr, wld_provider,
                  sound};
    if (tests) {
        if (auto code = tests->front_end(engine)) return *code;
    }

    // Instrumented mode: install SmokeTestHarness for interactive play (M166)
    std::unique_ptr<osc::lua::SmokeTestHarness> instrument_harness;
    if (instrument) {
        instrument_harness = std::make_unique<osc::lua::SmokeTestHarness>();
        instrument_harness->activate();
        // Install on ui_L (persistent)
        instrument_harness->install_panic_handler(ui_lua_state.raw());
        instrument_harness->install_global_interceptor(ui_lua_state.raw());
        instrument_harness->install_all_method_interceptors(ui_lua_state.raw());
        // Install on sim_L if it exists
        if (sim_lua_state) {
            instrument_harness->install_panic_handler(sim_lua_state->raw());
            instrument_harness->install_global_interceptor(sim_lua_state->raw());
            instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
        }
        spdlog::info("Instrumented mode active — smoke report on exit");
    }

    // Phase 5: Windowed mode (renderer) or headless tick loop
    if (!headless) {
        osc::renderer::Renderer renderer;
        // Scripted captures and checks render offscreen: no window to show,
        // focus to steal, or compositor to wait for.
        const bool offscreen_capture =
            !parse_string_arg(argc, argv, "--screenshot", "").empty() ||
            !parse_string_arg(argc, argv, "--golden", "").empty() || scripted_window;
        if (renderer.init(1600, 900, "OpenSupCom", offscreen_capture)) {
            // Build 3D scene if we have a sim state (--map was provided)
            if (sim_state) {
                renderer.build_scene(sim_state->terrain(), sim_state->blueprint_store(),
                                     osc::sim::world_blueprints(*sim_state), &vfs,
                                     ui_lua_state.raw());
            }

            // Store renderer pointer in UI Lua registry for WorldView/GetCamera
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_renderer");
                lua_pushlightuserdata(uL, &renderer);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Initialize texture/font caches for UI rendering (normally done in build_scene)
            if (!sim_state) {
                renderer.init_ui_caches(&vfs);
            }

            // Player input handler (ARMY_1 = index 0)
            osc::renderer::InputHandler input_handler;
            input_handler.set_player_army(0);
            renderer.set_player_army(0);
            // Store input_handler pointer in UI Lua registry for selection globals
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_input_handler");
                lua_pushlightuserdata(uL, &input_handler);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Factory queue display (M140c)
            osc::lua::FactoryQueueDisplay factory_queue;
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_factory_queue");
                lua_pushlightuserdata(uL, &factory_queue);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // SimCallback queue (UI→Sim bridge, M138a)
            osc::sim::SimCallbackQueue sim_callback_queue;
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_sim_callback_queue");
                lua_pushlightuserdata(uL, &sim_callback_queue);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Initialize hover entity ID to 0 (updated by WorldView HitTest, M142a)
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_hover_entity_id");
                lua_pushnumber(uL, 0);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Store scenario path for SessionGetScenarioInfo (M145c2)
            if (!map_path.empty()) {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_scenario_path");
                lua_pushstring(uL, map_path.c_str());
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // keymap_registry already stored in registry at outer scope

            // GameStateManager and BeatFunctionRegistry already declared at outer scope (M144b, M145b)

            if (no_fog) renderer.set_fog_enabled(false);
            if (no_decals) renderer.set_decals_enabled(false);
            if (legacy_hud) renderer.set_legacy_hud(true);

            double sim_accumulator = 0.0;
            std::optional<osc::sim::ReplayPlayback> active_playback; // a replay being watched
            double paused_beat_accumulator = 0.0;

            // FA's command mode drives world clicks (read once per frame).
            osc::renderer::CommandMode current_command_mode;
            bool ghost_from_mode = false;
            input_handler.set_command_mode_hooks(
                {[&] { return current_command_mode; },
                 [&](const osc::renderer::IssuedCommand& c) {
                     report_command_issued(ui_lua_state.raw(), c);
                 },
                 [&] { cancel_command_mode(ui_lua_state.raw()); }});
            auto prev_time = std::chrono::high_resolution_clock::now();
            bool p_was_pressed = false;
            bool plus_was_pressed = false;
            bool minus_was_pressed = false;
            bool esc_was_pressed = false;
            double title_update_timer = 0.0;
            double fps_accum = 0.0;
            int fps_frames = 0;
            double display_fps = 0.0;
            std::unordered_set<osc::u32> prev_selection;

            // Windowed LAN entry: create the transport up front so the front-end
            // can host/join while it renders; the game loop then drives the lobby
            // handshake to launch. Same networking path as --lan-host/--lan-join
            // (verified headless). Absent these flags, single-player is untouched.
            bool lan_launch_fired = false;
            bool lan_host_cfg_set = false;
            {
                std::string lwp = parse_string_arg(argc, argv, "--mp-port", "47624");
                auto lport = static_cast<osc::u16>(std::strtoul(lwp.c_str(), nullptr, 10));
                if (parse_flag(argc, argv, "--lan-window-host")) {
                    osc::lua::mp_begin_host(lport);
                }
                std::string lwj = parse_string_arg(argc, argv, "--lan-window-join", "");
                if (!lwj.empty()) osc::lua::mp_begin_join(lwj, lport);
            }

            // --screenshot <png> [--screenshot-frame N]: render N frames on a
            // fixed 60 Hz clock (so frame N is identical run to run), capture
            // the presented image, write it, and exit. Used for golden-image
            // tests and documentation shots.
            // --golden <name> [--golden-update]: capture like --screenshot and
            // compare against <golden dir>/<name>.png (OSC_GOLDEN_DIR, else the
            // user state dir). Goldens contain game art, so they live outside
            // the repository; a missing golden exits 77 (CTest "skipped").
            const std::string golden_name = parse_string_arg(argc, argv, "--golden", "");
            const bool golden_update = parse_flag(argc, argv, "--golden-update");
            osc::fs::path golden_path;
            if (!golden_name.empty()) {
                auto env_dir = osc::platform::system_env()("OSC_GOLDEN_DIR");
                osc::fs::path dir = env_dir && !env_dir->empty()
                    ? osc::fs::path(*env_dir)
                    : osc::platform::known_folder(osc::platform::KnownFolder::State) /
                          "opensupcom" / "golden";
                std::error_code ec;
                osc::fs::create_directories(dir, ec);
                golden_path = dir / (golden_name + ".png");
            }
            const std::string screenshot_path = !golden_name.empty()
                ? (golden_update ? golden_path.string()
                                 : (golden_path.parent_path() /
                                    (golden_name + ".actual.png")).string())
                : parse_string_arg(argc, argv, "--screenshot", "");
            const osc::u32 screenshot_frame = static_cast<osc::u32>(std::strtoul(
                parse_string_arg(argc, argv, "--screenshot-frame", "120").c_str(),
                nullptr, 10));
            constexpr double kScreenshotFrameDt = 1.0 / 60.0;
            // Scripted windowed runs (a test mode's, --replay-flow-test): four
            // frames per sim tick, on a fixed clock.
            constexpr double kInterpFrameDt = osc::sim::SimState::SECONDS_PER_TICK / 4.0;
            if (scripted_window) {
                renderer.set_fixed_frame_dt(static_cast<osc::f32>(kInterpFrameDt));
                renderer.camera().set_input_enabled(false);
            }
            osc::u32 frames_rendered = 0;
            bool screenshot_done = false;
            bool screenshot_ok = false;
            if (!screenshot_path.empty()) {
                renderer.set_fixed_frame_dt(static_cast<osc::f32>(kScreenshotFrameDt));
                // Golden images must not depend on where the mouse happens to be.
                renderer.camera().set_input_enabled(false);
            }
            // --camera <x>,<z>,<distance>: initial camera placement (world units).
            {
                const std::string cam = parse_string_arg(argc, argv, "--camera", "");
                float cx = 0, cz = 0, dist = 0;
                if (!cam.empty()) {
                    if (std::sscanf(cam.c_str(), "%f,%f,%f", &cx, &cz, &dist) == 3 &&
                        dist > 0) {
                        renderer.camera().set_target(cx, cz);
                        renderer.camera().set_distance(dist);
                    } else {
                        spdlog::error("--camera expects <x>,<z>,<distance>, got '{}'", cam);
                        return 1;
                    }
                }
            }

            // Open the replay (--watch, --replay-flow-test) through the same
            // globals retail's replay dialog calls; the loop then launches it.
            bool replay_flow_done = false;
            osc::u32 replay_flow_frames = 0;
            if (!watch_path.empty() || replay_flow_test) {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_watch_file");
                lua_pushstring(uL, watch_path.c_str());
                lua_rawset(uL, LUA_GLOBALSINDEX);
                auto opened = ui_lua_state.do_string(replay_flow_test ? R"(
                    local data = GetSpecialFiles('Replay')
                    local profile, name
                    for p, names in data.files do
                        if names[1] then
                            profile, name = p, names[1]
                            break
                        end
                    end
                    if not name then error('GetSpecialFiles lists no replay') end
                    local info = GetSpecialFileInfo(profile, name, 'Replay')
                    if not info or not info.WriteTime or not info.TimeStamp then
                        error('GetSpecialFileInfo does not describe ' .. name)
                    end
                    __osc_watch_file = data.directory .. profile .. '/' .. name .. '.' ..
                                       data.extension
                    if LaunchReplaySession(__osc_watch_file) ~= true then
                        error('LaunchReplaySession refused ' .. __osc_watch_file)
                    end
                )"
                                                                      : R"(
                    if LaunchReplaySession(__osc_watch_file) ~= true then
                        error('cannot play ' .. __osc_watch_file)
                    end
                )");
                if (!opened) {
                    spdlog::error("Replay: {}", opened.error().message);
                    if (replay_flow_test) {
                        osc::test_status::fail("[FAIL] replay-flow: {}", opened.error().message);
                        return finish_test_run("replay-flow-test");
                    }
                }
            }

            while (!renderer.should_close() && !screenshot_done &&
                   !(tests && tests->frames_done()) && !replay_flow_done) {
                osc::Profiler::instance().begin_frame();
                auto now = std::chrono::high_resolution_clock::now();
                double dt = std::chrono::duration<double>(now - prev_time).count();
                prev_time = now;
                if (!screenshot_path.empty()) dt = kScreenshotFrameDt;
                if (scripted_window) dt = kInterpFrameDt;
                // Clamp dt to avoid spiral of death
                if (dt > 0.25) dt = 0.25;

                // Audio: the camera is the listener, and FA's zoom and angle
                // curves read its distance and pitch.
                {
                    const auto& cam = renderer.camera();
                    osc::f32 ex = 0, ey = 0, ez = 0;
                    cam.eye_position(ex, ey, ez);
                    const osc::f32 fx = cam.target_x() - ex;
                    const osc::f32 fy = -ey;
                    const osc::f32 fz = cam.target_z() - ez;
                    const osc::f32 len = std::max(1e-3f, std::sqrt(fx * fx + fy * fy + fz * fz));
                    sound.set_listener({ex, ey, ez}, {fx / len, fy / len, fz / len});
                    sound.set_global_variable("CameraDistance", cam.distance());
                    const osc::f32 zoom_span = std::max(1.0f, cam.max_zoom() - cam.min_zoom());
                    sound.set_global_variable(
                        "ZoomPercent", 100.0f * (cam.distance() - cam.min_zoom()) / zoom_span);
                    sound.set_global_variable("Angle", cam.pitch() * 57.29578f);
                    sound.update(static_cast<osc::f32>(dt));
                }

                // FPS tracking
                fps_accum += dt;
                fps_frames++;
                if (fps_accum >= 0.5) {
                    display_fps = fps_frames / fps_accum;
                    fps_accum = 0.0;
                    fps_frames = 0;
                }

                // Pause toggle (P key, edge-triggered)
                bool p_pressed = renderer.is_key_pressed(GLFW_KEY_P);
                if (p_pressed && !p_was_pressed) {
                    game_state_mgr.set_paused(!game_state_mgr.paused(), ui_lua_state.raw());
                    spdlog::info("Sim {}", game_state_mgr.paused() ? "PAUSED" : "RESUMED");
                }
                p_was_pressed = p_pressed;

                // Speed control (+/- keys, edge-triggered)
                bool plus_pressed = renderer.is_key_pressed(GLFW_KEY_EQUAL) ||
                                    renderer.is_key_pressed(GLFW_KEY_KP_ADD);
                if (plus_pressed && !plus_was_pressed) {
                    game_state_mgr.set_speed(std::min(game_state_mgr.speed() * 2.0, 10.0));
                    spdlog::info("Sim speed: {:.1f}x", game_state_mgr.speed());
                }
                plus_was_pressed = plus_pressed;

                bool minus_pressed = renderer.is_key_pressed(GLFW_KEY_MINUS) ||
                                     renderer.is_key_pressed(GLFW_KEY_KP_SUBTRACT);
                if (minus_pressed && !minus_was_pressed) {
                    game_state_mgr.set_speed(std::max(game_state_mgr.speed() * 0.5, 0.125));
                    spdlog::info("Sim speed: {:.1f}x", game_state_mgr.speed());
                }
                minus_was_pressed = minus_pressed;

                // ESC key — call escape handler (M146c)
                bool esc_pressed = renderer.is_key_pressed(GLFW_KEY_ESCAPE);
                if (esc_pressed && !esc_was_pressed) {
                    lua_State* uiL = ui_lua_state.raw();
                    lua_pushstring(uiL, "__osc_escape_handler");
                    lua_rawget(uiL, LUA_REGISTRYINDEX);
                    if (lua_isfunction(uiL, -1)) {
                        if (lua_pcall(uiL, 0, 0, 0) != 0) {
                            spdlog::warn("ESC handler error: {}", lua_tostring(uiL, -1));
                            lua_pop(uiL, 1);
                        }
                    } else {
                        lua_pop(uiL, 1);
                    }
                }
                esc_was_pressed = esc_pressed;

                note_game_over_if_ended(sim_state.get(), game_state_mgr, ui_lua_state.raw());

                // Fixed-timestep sim ticking (scaled by sim_speed)
                const osc::u32 beat_tick0 = sim_state ? sim_state->tick_count() : 0;
                if (!game_state_mgr.paused() && !game_state_mgr.sim_stopped() && sim_state) {
                    world_interp.clock.advance(dt * game_state_mgr.speed());
                    if (osc::lua::mp_net_state().active()) {
                        // Multiplayer: advance in lockstep. Pace command frames
                        // at the sim tick rate; the session only advances the
                        // sim once every peer has confirmed the next frame
                        // (the classic "waiting for players" stall otherwise).
                        sim_accumulator += dt * game_state_mgr.speed();
                        auto* session = osc::lua::mp_net_state().session.get();
                        int guard = 0;
                        while (sim_accumulator >=
                                   osc::sim::SimState::SECONDS_PER_TICK &&
                               guard++ < 4) {
                            sim_accumulator -=
                                osc::sim::SimState::SECONDS_PER_TICK;
                            osc::lua::mp_pump(); // drain mux game channel + peers
                            session->send_frame();
                            session->receive_and_advance();
                            // A timed-out peer is defeated so the match resolves
                            // instead of stalling in "waiting for players": the
                            // survivors agree on its last frame, and the session
                            // defeats its army on the same tick on every one of
                            // them (a command, so replays keep it).
                            for (osc::u32 src : session->take_dropped())
                                spdlog::warn("[mp] peer {} dropped — its army is defeated", src);
                        }
                    } else {
                        // At most 8 ticks per frame; a slower-than-real-time
                        // sim slows the game rather than stalling every frame.
                        const int ticks = osc::consume_fixed_steps(
                            sim_accumulator, dt * game_state_mgr.speed(),
                            osc::sim::SimState::SECONDS_PER_TICK, 8);
                        for (int t = 0; t < ticks; ++t) {
                            // A replay plays to its end, then holds there.
                            if (active_playback && active_playback->finished(*sim_state)) break;
                            sim_state->tick();
                            if (active_playback) {
                                const bool diverged = active_playback->diverged_at() != 0;
                                if (!active_playback->check(*sim_state) && !diverged) {
                                    spdlog::warn("Replay: diverged from the recording at tick {}",
                                                 active_playback->diverged_at());
                                }
                            }
                        }
                    }
                }

                // Moho's sim beat reaches the user side once per tick, and
                // keeps running at the tick rate while paused.
                if (sim_state && sim_lua_state) {
                    osc::u32 beats = sim_state->tick_count() - beat_tick0;
                    if (game_state_mgr.paused()) {
                        paused_beat_accumulator += dt;
                        while (paused_beat_accumulator >=
                               osc::sim::SimState::SECONDS_PER_TICK) {
                            paused_beat_accumulator -=
                                osc::sim::SimState::SECONDS_PER_TICK;
                            ++beats;
                        }
                    } else {
                        paused_beat_accumulator = 0.0;
                    }
                    for (osc::u32 b = 0; b < beats; ++b)
                        world_beat(sim_lua_state.get(), sim_state.get(), ui_lua_state.raw());
                }

                // Process SimCallbacks from UI (M138a)
                if (sim_state && sim_lua_state)
                    submit_sim_callbacks(sim_callback_queue, *sim_state);

                // --replay-flow-test ends when the replay has played out.
                if (replay_flow_test) {
                    ++replay_flow_frames;
                    if (active_playback && sim_state && active_playback->finished(*sim_state))
                        replay_flow_done = true;
                    else if (replay_flow_frames > 40000)
                        replay_flow_done = true; // stuck: reported below
                }

                // OnFirstUpdate — fire once after first sim tick
                static bool first_update_fired = false;
                if (!first_update_fired && sim_state && sim_state->tick_count() > 0) {
                    osc::core::call_on_first_update(ui_lua_state.raw());
                    first_update_fired = true;
                }

                renderer.poll_events(dt);

                // Resume UI coroutines
                ++ui_frame_count;
                osc::lua::advance_ui_clock(ui_lua_state.raw(), dt);
                ui_thread_manager.resume_all(ui_frame_count);

                // OnBeat — UI heartbeat each frame
                osc::core::call_on_beat(ui_lua_state.raw(), dt);

                // Fire beat functions each frame (M145b)
                beat_registry.fire_all(ui_lua_state.raw());

                // This frame's world, between the last two ticks: what is
                // drawn and what clicks pick.
                const osc::sim::FrameView frame_view = world_interp.view();
                input_handler.set_frame_view(frame_view);
                if (tests && sim_state) {
                    Frame frame{frame_view, renderer, input_handler};
                    tests->frame_view(engine, frame);
                }

                // Player input: selection + commands
                if (sim_state) {
                current_command_mode = read_command_mode(ui_lua_state.raw());
                sync_build_ghost(*sim_state, current_command_mode, ghost_from_mode);
                input_handler.update(renderer, *sim_state, dt, [&] {
                    osc::f64 mx = 0, my = 0;
                    renderer.mouse_position(mx, my);
                    return mouse_over_ui(ui_lua_state.raw(), mx, my);
                });
                }

                dispatch_selection_change(ui_lua_state.raw(), prev_selection,
                                          input_handler.selected(),
                                          input_handler.take_selection_event());

                const auto& sel = input_handler.selected();
                if (!screenshot_path.empty() &&
                    ++frames_rendered == std::max<osc::u32>(screenshot_frame, 1)) {
                    const bool requested = renderer.request_capture(
                        [&](osc::ImageRGBA8 image) {
                            screenshot_ok = osc::write_png(screenshot_path, image);
                            screenshot_done = true;
                            spdlog::info("Screenshot {}x{} -> {} ({})", image.width,
                                         image.height, screenshot_path,
                                         screenshot_ok ? "written" : "WRITE FAILED");
                        });
                    if (!requested) {
                        spdlog::error("Screenshot: swapchain readback unsupported");
                        screenshot_done = true;
                    }
                }
                if (sim_state) {
                    const auto ghost = input_handler.build_ghost(renderer, *sim_state);
                    renderer.render(frame_view, world_interp.history.events(),
                                    ghost ? &*ghost : nullptr, ui_lua_state.raw(),
                                    &ui_registry, sel.empty() ? nullptr : &sel);
                    if (tests) {
                        Frame frame{frame_view, renderer, input_handler};
                        tests->frame_rendered(engine, frame);
                    }
                } else {
                    // No sim state (front-end/lobby) — render UI only
                    renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);
                }

                // Update window title periodically
                title_update_timer += dt;
                if (title_update_timer >= 0.25) {
                    title_update_timer = 0.0;
                    char title[256];
                    if (sim_state) {
                        const char* status_str =
                            game_state_mgr.game_over() ? "GAME OVER " :
                            game_state_mgr.paused() ? "PAUSED " : "";
                        std::snprintf(title, sizeof(title),
                            "OpenSupCom | %s%.1fx | T:%u (%.1fs) | %zu entities | %zu sel | %.0f FPS",
                            status_str,
                            game_state_mgr.speed(),
                            sim_state->tick_count(),
                            sim_state->game_time(),
                            sim_state->entity_registry().count(),
                            sel.size(),
                            display_fps);
                    } else {
                        std::snprintf(title, sizeof(title),
                            "OpenSupCom | Lobby | %.0f FPS", display_fps);
                    }
                    renderer.set_window_title(title);
                }

                // Check for exit request (M146d)
                {
                    lua_State* uiL = ui_lua_state.raw();
                    lua_pushstring(uiL, "__osc_exit_requested");
                    lua_rawget(uiL, LUA_REGISTRYINDEX);
                    if (lua_toboolean(uiL, -1)) {
                        lua_pop(uiL, 1);
                        break;
                    }
                    lua_pop(uiL, 1);
                }

                // LAN lobby: drive the host/client handshake during the
                // front-end and fire the launch barrier. Inert unless a LAN
                // transport exists (only when the LAN window flags were set).
                {
                    auto& mpn = osc::lua::mp_net_state();
                    auto* lob = osc::lua::mp_lobby();
                    if (lob && !mpn.active() && !lan_launch_fired) {
                        if (lob->role() == osc::lua::LanLobby::Role::Host &&
                            !lan_host_cfg_set) {
                            lob->set_host_config(osc::lua::LanSessionConfig{
                                "/maps/SCMP_009/SCMP_009_scenario.lua", mpn.seed});
                            lan_host_cfg_set = true;
                        }
                        osc::lua::mp_pump();
                        lob->poll();
                        if (lob->role() == osc::lua::LanLobby::Role::Host &&
                            lob->state() == osc::lua::LanLobby::State::Ready) {
                            lob->request_launch();
                        }
                        if (lob->launch_ready()) {
                            lan_launch_fired = true;
                            mpn.seed = lob->config().seed;
                            lan_launch_session(ui_lua_state.raw(),
                                               lob->config().scenario);
                        }
                    }
                }

                // Check for game launch request from lobby (M148a)
                {
                    lua_State* uiL = ui_lua_state.raw();
                    lua_pushstring(uiL, "__osc_launch_requested");
                    lua_rawget(uiL, LUA_REGISTRYINDEX);
                    if (lua_toboolean(uiL, -1)) {
                        lua_pop(uiL, 1);

                        // Clear the flag
                        lua_pushstring(uiL, "__osc_launch_requested");
                        lua_pushnil(uiL);
                        lua_rawset(uiL, LUA_REGISTRYINDEX);

                        // Read scenario path
                        lua_pushstring(uiL, "__osc_launch_scenario");
                        lua_rawget(uiL, LUA_REGISTRYINDEX);
                        const char* sc = lua_tostring(uiL, -1);
                        std::string launch_scenario = sc ? sc : "";
                        lua_pop(uiL, 1);

                        // A replay to play (LaunchReplaySession), if any
                        std::optional<osc::sim::Replay> launch_replay;
                        lua_pushstring(uiL, "__osc_launch_replay");
                        lua_rawget(uiL, LUA_REGISTRYINDEX);
                        if (lua_type(uiL, -1) == LUA_TSTRING) {
                            launch_replay = load_replay(lua_tostring(uiL, -1));
                            if (!launch_replay) launch_scenario.clear();
                        }
                        lua_pop(uiL, 1);
                        lua_pushstring(uiL, "__osc_launch_replay");
                        lua_pushnil(uiL);
                        lua_rawset(uiL, LUA_REGISTRYINDEX);

                        if (!launch_scenario.empty()) {
                            spdlog::info("Launch requested: {}{}", launch_scenario,
                                         launch_replay ? " (replay)" : "");
                            save_last_game(); // the game being left, if any
                            active_playback.reset();

                            // Transition to LOADING and show loading screen
                            game_state_mgr.transition_to(osc::GameState::LOADING, ui_lua_state.raw());
                            wld_provider.destroy_game_interface(ui_lua_state.raw());
                            begin_world_ui(ui_lua_state.raw(), wld_provider);

                            // Pump one UI frame to display loading screen
                            pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_count);
                            renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);

                            // Execute reload in stages, pumping UI frames between each
                            execute_reload_sequence(
                                sim_lua_state, sim_state, ui_lua_state, vfs, store, loader, config,
                                scenario_meta, game_state_mgr, &renderer, &input_handler,
                                &prev_selection, &world_interp,
                                launch_seed(seed_arg, reproducible_run), sim_accumulator,
                                launch_scenario, launch_replay ? &*launch_replay : nullptr);
                            if (launch_replay && sim_state) {
                                // Watched as an observer, as the replay plays.
                                active_playback.emplace(std::move(*launch_replay));
                                active_playback->start(*sim_state);
                                lua_pushstring(uiL, "__osc_focus_army");
                                lua_pushnumber(uiL, -1);
                                lua_rawset(uiL, LUA_REGISTRYINDEX);
                            }

                            // Reset per-session state for the new game
                            first_update_fired = false;

                            // Multiplayer: if the lobby stood up a network
                            // transport (HostGame/JoinGame), build the lockstep
                            // session over it now that the game's sim exists.
                            // No-op in single-player.
                            if (sim_state && !active_playback) {
                                osc::lua::mp_attach_session(*sim_state);
                            }

                            // Re-install instrument harness on new sim VM (M166)
                            if (instrument_harness && sim_lua_state) {
                                instrument_harness->install_panic_handler(sim_lua_state->raw());
                                instrument_harness->install_global_interceptor(sim_lua_state->raw());
                                instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
                            }

                            // Build the game interface; the loading dialog fades out
                            finish_world_ui(ui_lua_state.raw(), wld_provider,
                                            active_playback.has_value());
                        }
                    } else {
                        lua_pop(uiL, 1);
                    }
                }

                // Check for return-to-lobby request (M156b — score screen "Continue")
                {
                    lua_State* uiL = ui_lua_state.raw();
                    lua_pushstring(uiL, "__osc_return_to_lobby");
                    lua_rawget(uiL, LUA_REGISTRYINDEX);
                    if (lua_toboolean(uiL, -1)) {
                        lua_pop(uiL, 1);

                        // Clear the flag
                        lua_pushstring(uiL, "__osc_return_to_lobby");
                        lua_pushnil(uiL);
                        lua_rawset(uiL, LUA_REGISTRYINDEX);

                        spdlog::info("Returning to lobby...");

                        // Tear down any multiplayer session/transport before the
                        // sim it references is destroyed.
                        osc::lua::mp_teardown();
                        save_last_game();
                        active_playback.reset();

                        // Tear down game state
                        wld_provider.destroy_game_interface(uiL);
                        renderer.clear_scene();
                        detach_ui_from_sim(uiL);
                        sim_state.reset();
                        sim_lua_state.reset();

                        // Reset game state
                        game_state_mgr.set_game_over(false);
                        game_state_mgr.set_paused(false, uiL);
                        sim_accumulator = 0.0;
                        input_handler.set_selected({});
                        prev_selection.clear();

                        // Clear hover
                        lua_pushstring(uiL, "__osc_hover_entity_id");
                        lua_pushnumber(uiL, 0);
                        lua_rawset(uiL, LUA_REGISTRYINDEX);

                        // Transition to FRONT_END
                        game_state_mgr.transition_to(
                            osc::GameState::FRONT_END, uiL);

                        // Re-show lobby UI
                        osc::core::call_lua_global(uiL, "CreateUI");

                        // Rebuild the LAN dialog on the fresh front end.
                        {
                            lua_pushstring(uiL, "__osc_lan_dialog_built");
                            lua_pushnil(uiL);
                            lua_rawset(uiL, LUA_GLOBALSINDEX);
                            auto lr = ui_lua_state.do_string(osc::lua::kLanDialogLua);
                            if (!lr)
                                spdlog::warn("LAN dialog UI (relobby) error: {}",
                                             lr.error().message);
                        }

                        spdlog::info("=== Returned to lobby ===");
                    } else {
                        lua_pop(uiL, 1);
                    }
                }

                osc::Profiler::instance().end_frame();
            }

            save_last_game(); // quitting leaves the game being played
            renderer.shutdown();
            if (replay_flow_test) {
                auto is_replay = ui_lua_state.do_string(
                    "if not SessionIsReplay() then error('SessionIsReplay() is false') end");
                if (!active_playback || !sim_state) {
                    osc::test_status::fail("[FAIL] replay-flow: the replay never launched");
                } else if (!active_playback->finished(*sim_state)) {
                    osc::test_status::fail("[FAIL] replay-flow: stopped at tick {} of {}",
                                           sim_state->tick_count(),
                                           active_playback->replay().final_tick);
                } else if (active_playback->diverged_at() != 0) {
                    osc::test_status::fail("[FAIL] replay-flow: diverged at tick {}",
                                           active_playback->diverged_at());
                } else if (!is_replay) {
                    osc::test_status::fail("[FAIL] replay-flow: {}", is_replay.error().message);
                } else {
                    spdlog::info("[PASS] replay-flow: {} ticks watched in the game, matching "
                                 "the recording",
                                 sim_state->tick_count());
                }
                return finish_test_run("replay-flow-test");
            }
            if (tests) {
                if (auto code = tests->after_window()) return *code;
            }
            if (!screenshot_path.empty() &&
                osc::renderer::Renderer::validation_error_count() > 0) {
                spdlog::error("{} Vulkan validation error(s) during the capture run",
                              osc::renderer::Renderer::validation_error_count());
                return 1;
            }
            if (!golden_name.empty() && !golden_update) {
                if (!screenshot_ok) return 1;
                auto golden = osc::read_png(golden_path);
                if (!golden) {
                    spdlog::warn("No golden image at {} -- record one with "
                                 "--golden {} --golden-update", golden_path.string(),
                                 golden_name);
                    return kExitSkippedNoData;
                }
                auto actual = osc::read_png(screenshot_path);
                const double tolerance = std::strtod(
                    parse_string_arg(argc, argv, "--golden-tolerance", "0.01").c_str(),
                    nullptr);
                auto diff = osc::compare_images(*actual, *golden, 16);
                const bool pass = diff.same_size &&
                                  diff.fraction_over_threshold <= tolerance;
                spdlog::info("Golden '{}': {:.3f}% of pixels differ (tolerance "
                             "{:.3f}%), mean abs error {:.2f} -> {}",
                             golden_name, diff.fraction_over_threshold * 100.0,
                             tolerance * 100.0, diff.mean_abs_error,
                             pass ? "PASS" : "FAIL");
                if (!diff.same_size) {
                    spdlog::error("Golden '{}': size {}x{} != golden {}x{}", golden_name,
                                  actual->width, actual->height, golden->width,
                                  golden->height);
                }
                return pass ? 0 : 1;
            }
            if (!screenshot_path.empty()) return screenshot_ok ? 0 : 1;
        } else {
            if (parse_flag(argc, argv, "--screenshot") ||
                !parse_string_arg(argc, argv, "--screenshot", "").empty() ||
                !parse_string_arg(argc, argv, "--golden", "").empty()) {
                spdlog::error("Screenshot requested but the renderer failed to "
                              "initialize");
                return 1;
            }
            spdlog::warn("Vulkan init failed — falling back to headless "
                         "(100 ticks)");
            if (sim_state) {
                for (osc::u32 i = 0; i < 100; i++)
                    sim_state->tick();
            }
        }

        // Dump instrument report on exit (M166)
        if (instrument_harness) {
            instrument_harness->print_report(false);
            instrument_harness->write_report_to_file("smoke_report.txt", false);
            spdlog::info("Instrument report written to smoke_report.txt");
        }
    }

    if (tests) {
        if (auto code = tests->headless_first(engine)) return *code;
    }

    // === AI-vs-AI Skirmish (M163) ===
    if (ai_skirmish && !map_path.empty()) {
        osc::u32 max_ticks = tick_count > 0 ? tick_count : 6000; // default 10 min
        spdlog::info("=== AI-vs-AI Skirmish: up to {} ticks ({:.0f}s) ===",
                     max_ticks,
                     max_ticks * osc::sim::SimState::SECONDS_PER_TICK);

        osc::u32 ticks_run = 0;
        osc::i32 result = 0;
        osc::u32 log_interval = 100; // log stats every 10 game seconds

        osc::sim::SimRandom script_rng(0x5C817ED0);
        for (osc::u32 i = 0; i < max_ticks; i++) {
            if (scripted_orders) issue_scripted_orders(*sim_state, script_rng, i);
            sim_state->tick();
            ticks_run++;

            // Periodic stats logging
            if (ticks_run % log_interval == 0) {
                osc::u32 total_units = 0;
                osc::u32 total_vet = 0;
                for (size_t a = 0; a < sim_state->army_count(); a++) {
                    auto* brain = sim_state->army_at(a);
                    if (!brain || brain->is_civilian()) continue;
                    total_units += static_cast<osc::u32>(
                        brain->get_unit_cost_total(sim_state->entity_registry()));
                }
                sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                    if (!e.destroyed() && e.is_unit()) {
                        auto& u = static_cast<const osc::sim::Unit&>(e);
                        if (u.vet_level() > 0) total_vet++;
                    }
                });
                spdlog::info("  Tick {}: {:.1f}s | {} units alive | {} vetted | {} sounds",
                             ticks_run,
                             ticks_run * osc::sim::SimState::SECONDS_PER_TICK,
                             total_units, total_vet, sound.active_count());
            }

            // Check for game over
            result = sim_state->player_result();
            if (result != 0) {
                const char* result_str =
                    result == 1 ? "ARMY_1 WINS" :
                    result == 2 ? "ARMY_2 WINS" : "DRAW";
                spdlog::info("=== Game Over at tick {} ({:.1f}s): {} ===",
                             ticks_run,
                             ticks_run * osc::sim::SimState::SECONDS_PER_TICK,
                             result_str);
                break;
            }
        }

        if (result == 0) {
            spdlog::info("=== AI Skirmish: tick limit reached, no winner ===");
        }

        // Final summary
        spdlog::info("=== AI Skirmish Summary ===");
        spdlog::info("  Map: {}", map_path);
        spdlog::info("  Ticks: {} ({:.1f}s game time)",
                     ticks_run,
                     ticks_run * osc::sim::SimState::SECONDS_PER_TICK);
        spdlog::info("  Result: {}",
                     result == 1 ? "ARMY_1 wins" :
                     result == 2 ? "ARMY_2 wins" :
                     result == 0 ? "No winner (timeout)" : "Draw");
        for (size_t a = 0; a < sim_state->army_count(); a++) {
            auto* brain = sim_state->army_at(a);
            if (!brain || brain->is_civilian()) continue;
            osc::u32 surviving = 0;
            osc::u32 vetted = 0;
            osc::i32 army_idx = static_cast<osc::i32>(a);
            sim_state->entity_registry().for_each(
                [&](const osc::sim::Entity& e) {
                    if (!e.destroyed() && e.is_unit() && e.army() == army_idx) {
                        surviving++;
                        auto& u = static_cast<const osc::sim::Unit&>(e);
                        if (u.vet_level() > 0) vetted++;
                    }
                });
            spdlog::info("  Army {} ({}): {} units surviving, {} vetted",
                         a + 1, brain->is_defeated() ? "DEFEATED" : "alive",
                         surviving, vetted);
        }
        spdlog::info("=== End AI Skirmish ===");
    }

    // Headless tick loop
    if (!ai_skirmish && !map_path.empty() && tick_count > 0) {
        spdlog::info("Running {} sim ticks ({:.1f}s game time)...",
                     tick_count,
                     tick_count * osc::sim::SimState::SECONDS_PER_TICK);
        for (osc::u32 i = 0; i < tick_count; i++) {
            osc::Profiler::instance().begin_frame();
            sim_state->tick();
            osc::Profiler::instance().end_frame();
        }
    }


    if (tests) tests->headless(engine);

    // --dump-threads: where every live sim script thread is suspended.
    if (sim_state && parse_flag(argc, argv, "--dump-threads")) {
        for (const auto& line : sim_state->thread_manager().describe_threads()) {
            spdlog::info("[thread] {}", line);
        }
    }

    // Report final state
    if (sim_state) {
        spdlog::info("Sim: {} armies, {} entities, {} active threads, "
                     "{} ticks ({:.1f}s game time)",
                     sim_state->army_count(),
                     sim_state->entity_registry().count(),
                     sim_state->thread_manager().active_count(),
                     sim_state->tick_count(),
                     sim_state->game_time());
    }

    // Print profiling summary if enabled
    if (profile_enabled) {
        osc::Profiler::instance().log_summary();
    }

    const int exit_code = any_test ? finish_test_run("integration tests") : 0;
    recording_writer.write();
    osc::log::shutdown();
    return exit_code;
}

} // namespace osc::app
