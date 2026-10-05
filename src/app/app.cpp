#include "app/app_internal.hpp"
#include "app/log_bundle.hpp"
#include "core/log.hpp"
#include "core/test_status.hpp"
#include "core/version.hpp"
#include "lua/smoke_test.hpp"
#include "platform/crash_handler.hpp"

#include "sim/sim_callback_queue.hpp"
#include "sim/sim_snapshot.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
        opt->campaign_flow_test || opt->lan_game_test() || opt->gpgnet_scripted)
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
        const bool data_test_mode = opt->any_test || opt->lan_game_test() || opt->gpgnet_scripted ||
                                    opt->mods_flow_test || opt->campaign_flow_test ||
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
    // (a later save carries the whole game), and its checksum is the one
    // that history holds for the saved tick.
    sim_state->set_recording(true);
    sim_state->adopt_history(save.game);
    const auto& sums = save.game.checksums;
    const u64 at =
        save.tick >= save.game.checksum_from ? save.tick - save.game.checksum_from : sums.size();
    if (at >= sums.size() || sim_state->compute_sync_checksum() != sums[at]) {
        why = "it isn't the saved game (its checksum at tick " + std::to_string(save.tick) + ")";
        return Restore::Failed;
    }
    restored_at = save.tick;
    spdlog::info("Saved game '{}': restored at tick {} in {:.0f} ms", save.name, save.tick,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                     .count());
    post_load();
    return Restore::Done;
}

void App::post_load() {
    sim::SimCallbackEntry entry;
    entry.func_name = sim::kPostLoadCallback;
    sim_state->submit_callback(std::move(entry));
    if (world_ui_after_post_load) world_ui_after_post_load = sim_state->post_loads_run();
    spdlog::info("Saved game: its post-load follows tick {}", sim_state->tick_count());
}

bool App::check_catch_up() {
    if (!catch_up) return true;
    if (!catch_up->check(*sim_state)) {
        const u32 tick = catch_up->diverged_at();
        spdlog::error("Saved game: diverged from the save at tick {} as it caught up", tick);
        std::printf("LOAD diverged tick=%u\n", tick);
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

void App::save_last_game() {
    if (!opt.interactive || !sim_state || !sim_state->recording() || sim_state->playback()) return;
    const auto& replay = sim_state->recorded_replay();
    if (!replay.has_setup || replay.final_tick == 0) return;
    const std::string profile = prefs.get_string(prefs.current_profile_path() + ".Name", "Player");
    const auto* type = osc::lua::SpecialFiles::find_type("Replay");
    const auto path = special_files->path(*type, profile, "LastGame");
    if (path.empty()) {
        spdlog::warn("Replay: profile name '{}' can't be a folder name; LastGame not saved",
                     profile);
        return;
    }
    osc::lua::write_replay_file(replay, path);
}

} // namespace osc::app
