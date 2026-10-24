#include "app/app_internal.hpp"
#include "app/log_bundle.hpp"
#include "core/log.hpp"
#include "core/test_status.hpp"
#include "core/version.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/smoke_test.hpp"
#include "lua/special_files.hpp"
#include "platform/crash_handler.hpp"

#include "sim/sim_callback_queue.hpp"
#include "sim/sim_snapshot.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <cstring>
#include <memory>
#include <string_view>
#include <spdlog/spdlog.h>

namespace osc::app {

int run(int argc, char* argv[], TestModes* tests) {
    // Before the log starts: --version's one line is all a script reads,
    // and --collect-logs must not rotate the last game's log away
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("%s\n", osc::core::version_line());
            return 0;
        }
        if (std::strcmp(argv[i], "--collect-logs") == 0) {
            const bool named = i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0;
            return collect_logs(named ? argv[i + 1] : "");
        }
    }
    osc::log::init();
    osc::platform::install_crash_handler();
    // The log's file (M228b): --log's, or the integration runner's (its
    // working directory's), at once; the game's once its options say whose
    // game this is (open_run_log, below). Until then the lines are held.
    if (const auto file = parse_string_arg(argc, argv, "--log", ""); !file.empty())
        osc::log::open_file(file);
    else if (tests) osc::log::open_file("opensupcom.log");

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

    auto opt = parse_options(argc, argv, request);
    if (!opt) return 1;
    open_run_log(*opt);
    if (parse_flag(argc, argv, "--simulate-crash")) {
        spdlog::warn("--simulate-crash: crashing on purpose");
        osc::platform::crash_for_test();
    }
    g_record_path = parse_string_arg(argc, argv, "--record", "");
    // (A scripted windowed test mode counts its script errors if it says
    // so, in parse.)
    if (opt->any_test || opt->replay_flow_test || opt->load_flow_test || opt->mods_flow_test ||
        opt->self_destruct_flow_test || opt->restart_flow_test || opt->campaign_flow_test ||
        opt->lan_game_test() || opt->gpgnet_scripted)
        osc::test_status::set_count_lua_failures(true);

    // The first run (M228a): a player's game that found no FA asks where it
    // is, then looks again (never a test: nothing would answer)
    if ((config.fa_path.empty() || config.init_file.empty()) && may_ask_player(*opt) && !tests) {
        if (auto chosen = first_run_find_fa()) config = parse_args(argc, argv, tests, chosen);
    }

    if (config.fa_path.empty() || config.init_file.empty()) {
        spdlog::error("Supreme Commander: Forged Alliance not found. Pass "
                      "--fa-path <dir>, set OSC_FA_PATH, or run --print-install "
                      "to see where we looked.");
        const char* retail_only = std::getenv("OSC_RETAIL_ONLY"); // the gate's test switch
        const bool data_test_mode =
            tests || (retail_only && std::string_view(retail_only) == "1") || opt->any_test ||
            opt->lan_game_test() || opt->gpgnet_scripted || opt->mods_flow_test ||
            opt->self_destruct_flow_test || opt->restart_flow_test || opt->campaign_flow_test ||
            !parse_string_arg(argc, argv, "--binding-coverage", "").empty();
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

    if (opt->builder_debug) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::info("Builder debug mode enabled — verbose logging active");
    }

    App app(argc, argv, tests, std::move(config), request, std::move(*opt));
    return app.run();
}

App::App(int arg_count, char* arg_values[], TestModes* test_modes, lua::InitConfig init_config,
         TestRequest test_request, Options options)
    : argc(arg_count), argv(arg_values), tests(test_modes), config(std::move(init_config)),
      request(test_request), opt(std::move(options)),
      sim_lua_state(std::make_unique<lua::LuaState>()), store(sim_lua_state->raw()),
      // Audio: FA's sounds for the whole run (front end, lobby, games). Tests
      // and captures run it without an output device; headless runs have no
      // frames, so their sim tick is its clock.
      sound(config.fa_path / "sounds", !opt.headless && !opt.silent_capture),
      ui_store(ui_lua_state.raw()), ui_thread_manager(ui_lua_state.raw()),
      engine{config,
             opt.map_path,
             opt.tick_count,
             opt.seed_arg,
             opt.ai_personality,
             vfs,
             store,
             loader,
             sim_lua_state,
             sim_state,
             scenario_meta,
             ui_lua_state,
             ui_registry,
             ui_thread_manager,
             ui_frame_count,
             beat_registry,
             game_state_mgr,
             wld_provider,
             sound} {
    // Loading counts from here
    if (!opt.bench_report.empty()) bench.emplace(opt.bench_report);
    if (!opt.match_telemetry_path.empty()) telemetry.emplace(opt.match_telemetry_path);
}

App::~App() = default;

int App::run() {
    if (auto code = boot()) return *code;
    // Phase 5: Windowed mode (renderer) or headless tick loop
    if (!opt.headless) {
        if (auto code = run_window()) return *code;
    }
    return run_headless();
}

App::Restore App::restore_save(const sim::SavedGame& save, std::string& why) {
    // Asked to catch up from the save's history, the oracle a restore is
    // checked against: every load, headless or the game's own
    if (opt.load_by_replay) {
        why = "--load-by-replay";
        return Restore::Skipped;
    }
    if (!special_files) {
        why = "there are no special files";
        return Restore::Skipped;
    }
    if (!special_files->trusts_snapshot(save, why)) return Restore::Skipped;
    const auto start = std::chrono::steady_clock::now();
    if (std::string err = sim::load_snapshot(*sim_state, save.snapshot); !err.empty()) {
        why = err;
        return Restore::Failed;
    }
    // The restored game is the saved one: its history is its recording
    // (a later save carries the whole game; SessionIsBeingRecorded answers
    // false all the same, as Moho's for a loaded save), and its checksum is
    // the one that history holds for the saved tick.
    sim_state->set_recording(true);
    sim_state->adopt_history(save.game);
    const auto& sums = save.game.checksums;
    const u64 at =
        save.beat >= save.game.checksum_from ? save.beat - save.game.checksum_from : sums.size();
    if (at >= sums.size() || sim_state->compute_sync_checksum() != sums[at]) {
        why = "it isn't the saved game (its checksum at beat " + std::to_string(save.beat) + ")";
        return Restore::Failed;
    }
    restored_at = sim_state->tick_count();
    // Each peer discards queued network state beyond the agreed save beat.
    if (lua::mp_net_state().transport_ready) sim_state->command_scheduler().forget_network_state();
    spdlog::info("Saved game '{}': restored at tick {} in {:.0f} ms", save.name,
                 sim_state->tick_count(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                     .count());
    post_load();
    return Restore::Done;
}

void App::save_network_game(sim::SimState& sim, const std::string& name) {
    // (on the sim's thread, OE-134: no Lua but the sim's; the profile as it
    // was when the game began, and the player told between frames)
    const fs::path path =
        special_files ? lua::network_save_path(*special_files, network_profile, name) : fs::path();
    bool worked = false;
    if (path.empty() || !special_files) {
        spdlog::warn("Network game: not saved as '{}' (not a plain name, or no saves folder)",
                     name);
    } else {
        sim::SavedGame save = sim::save_game(sim, name);
        (void)special_files->sign_snapshot(save);
        worked = lua::write_saved_game(save, path);
        if (worked)
            spdlog::info("Network game saved as '{}' at tick {}: {}", name, save.beat,
                         path.string());
    }
    const std::lock_guard<std::mutex> lock(network_saves_mutex);
    network_save_notices.emplace_back(name, worked);
}

void App::deliver_network_save_notices() {
    std::vector<std::pair<std::string, bool>> notices;
    {
        const std::lock_guard<std::mutex> lock(network_saves_mutex);
        notices.swap(network_save_notices);
    }
    lua_State* uiL = ui_lua_state.raw();
    for (const auto& [name, worked] : notices) {
        // The UI's own word to the player, if its scripts have one
        lua_pushstring(uiL, "OnNetworkGameSaved");
        lua_rawget(uiL, LUA_GLOBALSINDEX);
        if (lua_isfunction(uiL, -1)) {
            lua_pushstring(uiL, name.c_str());
            lua_pushboolean(uiL, worked ? 1 : 0);
            if (lua_pcall(uiL, 2, 0, 0) != 0) {
                spdlog::warn("OnNetworkGameSaved: {}", lua_tostring(uiL, -1));
                lua_pop(uiL, 1);
            }
        } else {
            lua_pop(uiL, 1);
        }
    }
}

void App::post_load() {
    sim_state->schedule_post_load();
    if (world_ui_after_post_load) world_ui_after_post_load = sim_state->post_loads_run();
    spdlog::info("Saved game: its post-load follows tick {}", sim_state->tick_count());
}

bool App::check_catch_up() {
    if (!catch_up) return true;
    if (!catch_up->check(*sim_state)) {
        const u32 beat = catch_up->diverged_at();
        spdlog::error("Saved game: diverged from the save at beat {} as it caught up", beat);
        std::printf("LOAD diverged beat=%u\n", beat);
        catch_up.reset();
        return false;
    }
    if (!sim_state->resuming()) {
        spdlog::info("Saved game: caught up at tick {}; the game is the player's",
                     sim_state->tick_count());
        std::printf("LOAD resumed tick=%u\n", sim_state->tick_count());
        catch_up.reset();
        post_load();
    }
    return true;
}

std::filesystem::path App::recording_path(const std::string& base) const {
    const std::string profile = prefs.get_string(prefs.current_profile_path() + ".Name", "Player");
    const auto* type = osc::lua::SpecialFiles::find_type("Replay");
    return special_files->path(*type, profile, base);
}

void App::begin_match_record() {
    if (telemetry || !opt.interactive || !opt.match_telemetry_path.empty()) return;
    if (!sim_state || !sim_state->recording() || sim_state->playback()) return;
    auto path = recording_path("LastGame");
    if (path.empty()) return;
    path.replace_extension("jsonl");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    telemetry.emplace(path.string());
    match_record_open_ = telemetry->is_open();
    if (!match_record_open_) telemetry.reset();
}

namespace {

/// A kept game's name: when it ended, and its map (`2026-10-13 2117
/// SCMP_009`): one plain file name, in order of time when sorted.
std::string kept_game_name(const std::string& scenario) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char when[32];
    (void)std::strftime(when, sizeof when, "%Y-%m-%d %H%M%S", &local);
    std::string map = std::filesystem::path(scenario).parent_path().filename().string();
    for (char& c : map)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
    return map.empty() ? std::string(when) : std::string(when) + " " + map;
}

} // namespace

void App::save_last_game() {
    if (!opt.interactive || !sim_state || !sim_state->recording() || sim_state->playback()) return;
    const auto& replay = sim_state->recorded_replay();
    // The game's statistics end with it, whatever becomes of the recording
    const bool had_record = match_record_open_;
    if (had_record) {
        telemetry->finish(sim_state.get(), "ended");
        telemetry.reset();
        match_record_open_ = false;
    }
    if (!replay.has_setup || replay.final_beat == 0) return;
    const auto path = recording_path("LastGame");

    if (path.empty()) {
        spdlog::warn("Replay: profile's name can't be a folder name; LastGame not saved");
        return;
    }
    osc::lua::write_replay_file(replay, path);
    // And kept under a name of its own, its statistics beside it
    const auto kept = recording_path(kept_game_name(replay.setup.scenario));
    if (kept.empty()) return;
    std::error_code ec;
    std::filesystem::copy_file(path, kept, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::warn("Replay: the game was not kept as {}: {}", kept.string(), ec.message());
        return;
    }
    if (had_record) {
        auto stats = path;
        stats.replace_extension("jsonl");
        auto kept_stats = kept;
        kept_stats.replace_extension("jsonl");
        std::filesystem::copy_file(stats, kept_stats,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    spdlog::info("Replay: the game is kept as {}", kept.string());
}

} // namespace osc::app
