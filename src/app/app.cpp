#include "app/app_internal.hpp"
#include "core/log.hpp"
#include "core/test_status.hpp"
#include "lua/smoke_test.hpp"
#include "platform/crash_handler.hpp"

#include <cstdio>
#include <memory>
#include <spdlog/spdlog.h>

namespace osc::app {

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

    auto opt = parse_options(argc, argv, request);
    if (!opt) return 1;
    g_record_path = parse_string_arg(argc, argv, "--record", "");
    // (A scripted windowed test mode counts its script errors if it says
    // so, in parse.)
    if (opt->any_test || opt->replay_flow_test || opt->load_flow_test)
        osc::test_status::set_count_lua_failures(true);

    if (config.fa_path.empty() || config.init_file.empty()) {
        spdlog::error("Supreme Commander: Forged Alliance not found. Pass "
                      "--fa-path <dir>, set OSC_FA_PATH, or run --print-install "
                      "to see where we looked.");
        const bool data_test_mode =
            opt->any_test || !parse_string_arg(argc, argv, "--binding-coverage", "").empty();
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
             sound} {}

App::~App() = default;

int App::run() {
    if (auto code = boot()) return *code;
    // Phase 5: Windowed mode (renderer) or headless tick loop
    if (!opt.headless) {
        if (auto code = run_window()) return *code;
    }
    return run_headless();
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
