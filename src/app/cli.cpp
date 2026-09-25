// The command line: usage and the arguments the application reads (M192
// step 2, moved from app.cpp).

#include "app/app_internal.hpp"
#include "platform/game_install.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <spdlog/spdlog.h>

namespace osc::app {

void print_usage() {
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

osc::lua::InitConfig parse_args(int argc, char* argv[], const TestModes* tests) {
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

/// A test mode's flag given to the game (the integration runner has them),
/// or null.
const char* test_mode_flag(int argc, char* argv[]) {
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

} // namespace osc::app
