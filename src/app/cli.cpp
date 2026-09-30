// The command line: usage and the arguments the application reads (M192
// step 2, moved from app.cpp).

#include "app/app_internal.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "platform/crash_handler.hpp"
#include "platform/engine_settings.hpp"
#include "platform/first_run.hpp"
#include "platform/game_install.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <spdlog/spdlog.h>

namespace osc::app {

namespace {

/// How many earlier runs' logs a player's game keeps (M228b).
constexpr int kKeptLogs = 5;

} // namespace

void print_usage() {
    // The option table is laid out by hand.
    // clang-format off
    std::cout << osc::core::version_line() << "\n"
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
              << "  --camera <x>,<z>,<zoom> Initial camera target and zoom\n"
              << "  --legacy-hud       Draw the C++ HUD placeholders over FA's game interface\n"
              << "  --prefs <path>     Game.prefs to use (default: the user's config dir;\n"
              << "                     tests and captures keep preferences in memory)\n"
              << "  --golden <name>    Capture like --screenshot, compare to golden image\n"
              << "  --golden-update    Record the golden image instead of comparing\n"
              << "  --click <label>    Click the button so labelled once it takes a click;\n"
              << "                     repeat for the next (captures of retail's menus)\n"
              << "  --binding-coverage <file>  Report engine API the scripts call but\n"
              << "                     the engine lacks (needs --map)\n"
              << "  --binding-baseline <file>  With --binding-coverage: fail on gaps\n"
              << "                     not listed in this baseline\n"
              << "  --dump-threads     At exit, list where each sim script thread waits\n"
              << "  --ai-armies <n>    With --ai-skirmish: number of AI armies (default 2)\n"
              << "  --map <vfs-path>   VFS path to *_scenario.lua (a campaign operation's\n"
              << "                     launches as the campaign does)\n"
              << "  --difficulty <1-3> A campaign operation's difficulty (default 2)\n"
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
              << "  --load-flow-test   Offscreen: load the first listed saved game as retail's\n"
              << "                     Load dialog does, play on, and save it again\n"
              << "  --mods-flow-test   Offscreen: a skirmish with the player's mods, launched\n"
              << "                     as retail's lobby launches one, reporting what they did\n"
              << "  --mods-flow-lobby  ...through retail's lobby, picking Resource Rich in its\n"
              << "                     mod manager with a player's clicks\n"
              << "  --replay <file>    Play a recorded game headlessly, checking every tick's\n"
              << "                     checksum against the recording (exit 1 on divergence)\n"
              << "  --load <file>      Load a saved game: with --ticks or --ai-skirmish it\n"
              << "                     restores its snapshot (or catches up; exit 1 on\n"
              << "                     divergence) headlessly and plays on; else the game\n"
              << "                     opens it\n"
              << "  --load-by-replay   ...catching up from its history even with a snapshot\n"
              << "  --save <file> --save-at <tick>  Headless: save the game after that tick\n"
              << "  --scripted-orders  With --ai-skirmish: army 1 also takes a player's\n"
              << "                     orders (moves, pauses, fire states, stops), and one\n"
              << "                     more just before --save-at's save\n"
              << "  --bench <file>     With --ticks or --ai-skirmish: write each tick's time\n"
              << "                     and the game's checksum as JSON (tools/bench.py)\n"
              << "  --render-bench <file>  With --load: render a scene of the saved game\n"
              << "                     offscreen at 1920x1080 and write its frames' figures\n"
              << "                     as JSON (M223b; tools/bench.py)\n"
              << "  --render-scene <battle|late|strategic>  The scene (default battle)\n"
              << "  --render-frames <n> --render-warmup <n>  Frames measured (default 600),\n"
              << "                     after the warm-up's (default 120)\n"
              << "  --profile          Enable performance profiling (prints summary at exit)\n"
              << "  --instrument       Interactive instrumented mode (smoke report on exit)\n"
              << "  --log <file>       Write the log there (default: a player's game logs to\n"
              << "                     the user's state folder, anything else to ./opensupcom.log)\n"
              << "  --collect-logs [zip]  Zip the logs, crash reports and settings for a bug\n"
              << "                     report, and exit\n"
              << "  --simulate-crash   Crash on purpose, to check crash reports\n"
              << "  --version          Print the version and exit\n"
              << "  --help             Show this help message\n";
    // clang-format on
}

osc::lua::InitConfig parse_args(int argc, char* argv[], const TestModes* tests,
                                const std::optional<fs::path>& chosen) {
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

    // The folder the player chose when nothing was found (M228a): the last
    // place looked
    const auto env = osc::platform::system_env();
    hints.chosen_fa_path =
        chosen
            ? chosen
            : osc::platform::load_engine_settings(osc::platform::engine_settings_path(env)).fa_path;
    auto search = osc::platform::locate_game_install(hints, env);

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

bool may_ask_player(const Options& opt) {
    return opt.interactive && !opt.scripted_window;
}

void open_run_log(const Options& opt) {
    if (osc::log::file_open()) return;
    if (may_ask_player(opt)) {
        const auto env = osc::platform::system_env();
        const auto file = osc::platform::engine_log_file(env);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        osc::log::rotate(file, kKeptLogs);
        if (osc::log::open_file(file)) {
            spdlog::info("Log: {}", file.string());
            osc::platform::set_crash_report_dir(osc::platform::engine_crash_dir(env),
                                                std::string(osc::core::version_line()) +
                                                    "\nLog: " + file.string());
            return;
        }
    }
    osc::log::open_file("opensupcom.log");
}

std::optional<fs::path> first_run_find_fa() {
    auto prompter = osc::platform::make_native_prompter();
    if (!prompter) {
        spdlog::warn("No dialog to ask where FA is with (Linux: install zenity or kdialog)");
        return std::nullopt;
    }
    const auto env = osc::platform::system_env();
    const auto settings = osc::platform::engine_settings_path(env);
    const auto result =
        osc::platform::ask_for_fa_install(*prompter, settings, osc::platform::home_dir(env));
    if (!result.fa_path) return std::nullopt;
    spdlog::info("FA folder chosen: {}", result.fa_path->string());
    if (!result.saved)
        spdlog::warn("Couldn't save it in {}: the next start will ask again", settings.string());
    return result.fa_path;
}

osc::u32 parse_ticks_arg(int argc, char* argv[]) {
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

std::string parse_map_arg(int argc, char* argv[]) {
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

std::optional<Options> parse_options(int argc, char* argv[], const TestRequest& request) {
    Options o;
    o.map_path = parse_map_arg(argc, argv);
    o.tick_count = parse_ticks_arg(argc, argv);
    // --replay <file>: play a recorded game (its setup names the map).
    if (const auto replay_arg = parse_string_arg(argc, argv, "--replay", ""); !replay_arg.empty()) {
        o.replay_to_play = load_replay(replay_arg);
        if (!o.replay_to_play) return std::nullopt;
        o.map_path = o.replay_to_play->setup.scenario;
    }
    o.scripted_orders = parse_flag(argc, argv, "--scripted-orders");
    o.load_by_replay = parse_flag(argc, argv, "--load-by-replay");
    o.bench_report = parse_string_arg(argc, argv, "--bench", "");
    o.render_bench_report = parse_string_arg(argc, argv, "--render-bench", "");
    o.render_scene = parse_string_arg(argc, argv, "--render-scene", "battle");
    o.render_frames = static_cast<u32>(
        std::strtoul(parse_string_arg(argc, argv, "--render-frames", "600").c_str(), nullptr, 10));
    o.render_warmup = static_cast<u32>(
        std::strtoul(parse_string_arg(argc, argv, "--render-warmup", "120").c_str(), nullptr, 10));
    // Scripted runs of the windowed loop: offscreen, silent, fixed clock.
    // --watch <file>: open a replay in the game, as the replay dialog does.
    // --replay-flow-test: the dialog's own path (the first replay
    // GetSpecialFiles lists), played to its end offscreen.
    o.watch_path = parse_string_arg(argc, argv, "--watch", "");
    o.replay_flow_test = parse_flag(argc, argv, "--replay-flow-test");
    // --load-flow-test: the Load dialog's path (the first saved game
    // GetSpecialFiles lists), caught up and saved again, offscreen.
    o.load_flow_test = parse_flag(argc, argv, "--load-flow-test");
    // --mods-flow-test: a skirmish with the player's mods (M221b).
    o.mods_flow_test = parse_flag(argc, argv, "--mods-flow-test");
    o.mods_flow_lobby = parse_flag(argc, argv, "--mods-flow-lobby"); // (M221c)
    // --click <label>, repeatable: the buttons a player clicks, in order
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--click") == 0) {
            o.clicks.emplace_back(argv[++i]);
        }
    }
    // --lan-game-host / --lan-game-join <address> (--mp-port <port>): two
    // processes play retail's LAN lobby to a game.
    o.lan_game_host = parse_flag(argc, argv, "--lan-game-host");
    o.lan_game_join = parse_string_arg(argc, argv, "--lan-game-join", "");
    if (const auto port = parse_string_arg(argc, argv, "--mp-port", "");
        !port.empty() && port.size() <= 5 &&
        port.find_first_not_of("0123456789") == std::string::npos)
        o.lan_game_port = static_cast<u16>(std::clamp(std::stoi(port), 1, 65535));
    // --lan-game-quit-at <tick>: the joiner leaves the game then (M218e)
    if (const auto quit = parse_string_arg(argc, argv, "--lan-game-quit-at", "");
        !quit.empty() && quit.size() <= 6 &&
        quit.find_first_not_of("0123456789") == std::string::npos)
        o.lan_game_quit_at = static_cast<u32>(std::stoi(quit));
    // /gpgnet host:port (Moho's spelling, as FAF's client passes it)
    o.gpgnet_endpoint = parse_string_arg(argc, argv, "/gpgnet", "");
    if (o.gpgnet_endpoint.empty()) o.gpgnet_endpoint = parse_string_arg(argc, argv, "--gpgnet", "");
    o.gpgnet_scripted = !o.gpgnet_endpoint.empty() && parse_flag(argc, argv, "--gpgnet-scripted");
    o.scripted_window = request.windowed || o.replay_flow_test || o.load_flow_test ||
                        o.mods_flow_test || !o.clicks.empty() || o.lan_game_test() ||
                        o.gpgnet_scripted;
    o.no_fog = parse_flag(argc, argv, "--no-fog");
    o.legacy_hud = parse_flag(argc, argv, "--legacy-hud");
    o.no_decals = parse_flag(argc, argv, "--no-decals");
    o.profile_enabled = parse_flag(argc, argv, "--profile");
    o.ai_skirmish = parse_flag(argc, argv, "--ai-skirmish");
    o.instrument = parse_flag(argc, argv, "--instrument");
    o.builder_debug = parse_flag(argc, argv, "--builder-debug");
    o.ai_personality = parse_string_arg(argc, argv, "--ai-personality", "adaptive");
    // --ai-armies <n>: how many of the scenario's armies play (all AI).
    if (const auto n = parse_string_arg(argc, argv, "--ai-armies", ""); !n.empty()) {
        o.ai_army_count = static_cast<size_t>(std::max(1, std::atoi(n.c_str())));
    }

    // The command line, for HasCommandLineArg (M147d) and GetCommandLineArg
    for (int i = 1; i < argc; ++i) o.cmdline_args.emplace_back(argv[i]);

    // A checked run: headless, and its exit code is the checks' result (a
    // test mode, or an AI game whose script errors count).
    o.any_test = request.headless || o.ai_skirmish;
    o.headless = (o.tick_count > 0) || o.any_test || o.replay_to_play.has_value();

    o.silent_capture = !parse_string_arg(argc, argv, "--screenshot", "").empty() ||
                       !parse_string_arg(argc, argv, "--golden", "").empty() || o.scripted_window;
    // --seed N: the game's random seed (weapon spread, scripts' Random and
    // math.random). Runs that must repeat default to a fixed one.
    o.seed_arg = parse_string_arg(argc, argv, "--seed", "");
    o.reproducible_run = o.headless || o.scripted_window || o.silent_capture;
    o.interactive = !o.headless && parse_string_arg(argc, argv, "--screenshot", "").empty() &&
                    parse_string_arg(argc, argv, "--golden", "").empty();
    // --load <file>: a saved game. A headless run (--ticks, --ai-skirmish)
    // catches it up and plays on; else the window opens it.
    o.load_path = parse_string_arg(argc, argv, "--load", "");
    if (!o.load_path.empty() && o.headless) {
        sim::SavedGame save;
        if (lua::read_saved_game(o.load_path, save) != sim::SaveLoadError::None)
            return std::nullopt;
        o.map_path = save.game.setup.scenario;
        o.save_to_load = std::move(save);
        o.load_path.clear();
    }
    // --save <file> --save-at <tick>: save the game after that tick.
    o.save_path = parse_string_arg(argc, argv, "--save", "");
    o.save_at = static_cast<u32>(
        std::strtoul(parse_string_arg(argc, argv, "--save-at", "0").c_str(), nullptr, 10));
    if (!o.save_path.empty() && (o.save_at == 0 || !o.headless)) {
        // A headless run saves after a tick: its checks run after each one.
        spdlog::error("--save needs --save-at <tick> (1 or more), and --ticks or --ai-skirmish");
        return std::nullopt;
    }
    return o;
}

/// A test mode's flag given to the game (the integration runner has them),
/// or null.
const char* test_mode_flag(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool test = arg.size() > 7 && arg.starts_with("--") && arg.ends_with("-test") &&
                          arg != "--replay-flow-test" && arg != "--load-flow-test" &&
                          arg != "--mods-flow-test";
        if (test || arg == "--render-dump" || arg == "--mp-host" || arg == "--mp-join")
            return argv[i];
    }
    return nullptr;
}

} // namespace osc::app
