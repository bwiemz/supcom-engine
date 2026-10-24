#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "platform/engine_settings.hpp"
#include "platform/first_run.hpp"
#include "platform/game_install.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace osc::platform;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("osc_first_run_test_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << contents;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// An FA install as the first run checks for one.
fs::path make_fa(const fs::path& dir) {
    write_file(dir / "bin" / "SupComDataPath.lua", "path = {}\n");
    write_file(dir / "gamedata" / "lua.scd", "");
    return dir;
}

EnvLookup fake_env(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* key) -> std::optional<std::string> {
        auto it = vars.find(key);
        if (it == vars.end()) return std::nullopt;
        return it->second;
    };
}

/// The player, scripted: each question's answer and each folder pick, in turn.
class ScriptedPrompter final : public Prompter {
public:
    std::deque<bool> answers;
    std::deque<std::optional<fs::path>> picks;
    std::vector<std::string> asked; ///< each question's text
    std::vector<fs::path> starts;   ///< where each pick started

    bool ask(const std::string& /*title*/, const std::string& text) override {
        asked.push_back(text);
        REQUIRE_FALSE(answers.empty());
        const bool answer = answers.front();
        answers.pop_front();
        return answer;
    }

    std::optional<fs::path> pick_folder(const std::string& /*title*/,
                                        const fs::path& start) override {
        starts.push_back(start);
        REQUIRE_FALSE(picks.empty());
        auto pick = picks.front();
        picks.pop_front();
        return pick;
    }
};

} // namespace

TEST_CASE("fa_install_problem says what a folder lacks", "[platform][first_run]") {
    TempDir tmp;
    CHECK(fa_install_problem(tmp.path / "nowhere") == "it isn't a folder");
    CHECK(fa_install_problem(tmp.path) == "it has no bin/SupComDataPath.lua");
    write_file(tmp.path / "bin" / "SupComDataPath.lua", "");
    CHECK(fa_install_problem(tmp.path) == "it has no gamedata/lua.scd");
    write_file(tmp.path / "gamedata" / "lua.scd", "");
    CHECK_FALSE(fa_install_problem(tmp.path).has_value());
}

TEST_CASE("fa_install_problem takes FA's files in any case", "[platform][first_run]") {
    // FA's files were written on Windows; a Linux copy may keep any case
    TempDir tmp;
    write_file(tmp.path / "BIN" / "supcomdatapath.LUA", "");
    write_file(tmp.path / "GameData" / "LUA.SCD", "");
    CHECK_FALSE(fa_install_problem(tmp.path).has_value());
}

TEST_CASE("resolve_fa_folder takes FA's bin or gamedata folder for FA", "[platform][first_run]") {
    TempDir tmp;
    const fs::path fa = make_fa(tmp.path / "Forged Alliance");
    CHECK(resolve_fa_folder(fa) == fa);
    CHECK(resolve_fa_folder(fa / "bin") == fa);
    CHECK(resolve_fa_folder(fa / "gamedata") == fa);
    CHECK_FALSE(resolve_fa_folder(tmp.path).has_value());
    // A "bin" folder elsewhere isn't FA's
    fs::create_directories(tmp.path / "other" / "bin");
    CHECK_FALSE(resolve_fa_folder(tmp.path / "other" / "bin").has_value());
}

TEST_CASE("Engine settings round-trip, keeping what another version wrote",
          "[platform][first_run]") {
    TempDir tmp;
    const fs::path file = tmp.path / "config" / "opensupcom" / "settings.json";
    CHECK_FALSE(load_engine_settings(file).fa_path.has_value()); // none yet

    write_file(file, R"({"future_key": [1, 2, 3], "fa_path": "/old"})");
    EngineSettings settings = load_engine_settings(file);
    REQUIRE(settings.fa_path.has_value());
    CHECK(*settings.fa_path == fs::path("/old"));

    const fs::path fa = tmp.path / "Games" / "Forged Alliance é";
    settings.fa_path = fa;
    REQUIRE(save_engine_settings(file, settings));
    CHECK(load_engine_settings(file).fa_path == fa);
    CHECK(read_file(file).find("future_key") != std::string::npos);
    CHECK_FALSE(fs::exists(fs::path(file.string() + ".tmp")));

    settings.fa_path.reset();
    REQUIRE(save_engine_settings(file, settings));
    CHECK_FALSE(load_engine_settings(file).fa_path.has_value());
    CHECK(read_file(file).find("future_key") != std::string::npos);
}

TEST_CASE("A broken settings file reads as defaults and is replaced when saved",
          "[platform][first_run]") {
    TempDir tmp;
    const fs::path file = tmp.path / "settings.json";
    for (const char* broken : {"{ not json", "[\"an\", \"array\"]", "{\"fa_path\": 7}", ""}) {
        write_file(file, broken);
        CHECK_FALSE(load_engine_settings(file).fa_path.has_value());
    }
    EngineSettings settings;
    settings.fa_path = tmp.path / "fa";
    REQUIRE(save_engine_settings(file, settings));
    CHECK(load_engine_settings(file).fa_path == tmp.path / "fa");
}

TEST_CASE("The first run asks until it gets FA, then saves it", "[platform][first_run]") {
    TempDir tmp;
    const fs::path fa = make_fa(tmp.path / "Forged Alliance");
    const fs::path settings = tmp.path / "settings.json";
    ScriptedPrompter player;
    // Yes, a wrong folder, yes (another), FA's bin folder
    player.answers = {true, true};
    player.picks = {tmp.path / "Documents", fa / "bin"};
    fs::create_directories(tmp.path / "Documents");

    const auto result = ask_for_fa_install(player, settings, tmp.path);
    REQUIRE(result.fa_path.has_value());
    CHECK(*result.fa_path == fa);
    CHECK(result.saved);
    CHECK(load_engine_settings(settings).fa_path == fa);
    // It said what was wrong with the first folder, and the second pick
    // started from it
    REQUIRE(player.asked.size() == 2);
    CHECK(player.asked[1].find("it has no bin/SupComDataPath.lua") != std::string::npos);
    REQUIRE(player.starts.size() == 2);
    CHECK(player.starts[0] == tmp.path);
    CHECK(player.starts[1] == tmp.path / "Documents");
}

TEST_CASE("The first run saves nothing when the player cancels", "[platform][first_run]") {
    TempDir tmp;
    const fs::path settings = tmp.path / "settings.json";
    fs::create_directories(tmp.path / "Documents");

    SECTION("the question") {
        ScriptedPrompter player;
        player.answers = {false};
        CHECK_FALSE(ask_for_fa_install(player, settings, tmp.path).fa_path.has_value());
        CHECK(player.starts.empty());
    }
    SECTION("the folder picker") {
        ScriptedPrompter player;
        player.answers = {true};
        player.picks = {std::nullopt};
        CHECK_FALSE(ask_for_fa_install(player, settings, tmp.path).fa_path.has_value());
    }
    SECTION("another try, after a wrong folder") {
        ScriptedPrompter player;
        player.answers = {true, false};
        player.picks = {tmp.path / "Documents"};
        CHECK_FALSE(ask_for_fa_install(player, settings, tmp.path).fa_path.has_value());
    }
    CHECK_FALSE(fs::exists(settings));
}

#ifndef _WIN32

TEST_CASE("The engine's settings live in the config folder", "[platform][first_run]") {
    auto env = fake_env({{"HOME", "/home/p"}, {"XDG_CONFIG_HOME", "/home/p/.cfg"}});
    CHECK(engine_settings_path(env) == fs::path("/home/p/.cfg/opensupcom/settings.json"));
}

TEST_CASE("The chosen FA folder is the search's last resort", "[platform][first_run]") {
    TempDir tmp;
    const fs::path chosen = make_fa(tmp.path / "chosen");
    auto empty_home = fake_env({{"HOME", (tmp.path / "empty_home").string()}});

    GameInstallHints hints;
    hints.chosen_fa_path = chosen;
    auto found = locate_game_install(hints, empty_home);
    REQUIRE(found.install.has_value());
    CHECK(found.install->source == "chosen");
    CHECK(found.install->fa_path == chosen);
    CHECK(found.install->init_file == chosen / "bin" / "SupComDataPath.lua");

    // FAF (or Steam) found later takes over
    const fs::path faf = tmp.path / "empty_home" / ".faforever";
    write_file(faf / "bin" / "init_faf.lua", "-- faf\n");
    CHECK(locate_game_install(hints, empty_home).install->source == "faf");
    fs::remove_all(faf);

    // ...and so does the command line
    GameInstallHints cli = hints;
    cli.fa_path = make_fa(tmp.path / "cli");
    CHECK(locate_game_install(cli, empty_home).install->source == "cli");

    // A chosen folder FA has gone from is passed over, and said so
    hints.chosen_fa_path = tmp.path / "gone";
    auto gone = locate_game_install(hints, empty_home);
    CHECK_FALSE(gone.install.has_value());
    CHECK(gone.searched.back() == "chosen: " + (tmp.path / "gone").string());
}

#endif

TEST_CASE("Only a player's own game stops to ask where FA is", "[app][first_run]") {
    // The flow tests and LAN games run the game binary itself, in a window,
    // with nothing to answer a dialog: they skip when FA isn't found
    const auto asks = [](std::vector<std::string> args) {
        args.insert(args.begin(), "opensupcom");
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& a : args) argv.push_back(a.data());
        const auto opt = osc::app::parse_options(static_cast<int>(argv.size()), argv.data(), {});
        REQUIRE(opt);
        return osc::app::may_ask_player(*opt);
    };
    CHECK(asks({}));
    CHECK(asks({"--map", "/maps/SCMP_009/SCMP_009_scenario.lua"}));
    CHECK_FALSE(asks({"--ticks", "5"}));
    CHECK_FALSE(asks({"--ai-skirmish"}));
    CHECK_FALSE(asks({"--replay-flow-test"}));
    CHECK_FALSE(asks({"--load-flow-test"}));
    CHECK_FALSE(asks({"--lan-game-host"}));
    CHECK_FALSE(asks({"--lan-game-join", "127.0.0.1"}));
    CHECK_FALSE(asks({"--screenshot", "shot.png"}));
    CHECK_FALSE(asks({"--golden", "name"}));
}

namespace {

/// `args` after the program's name, as main is given them.
struct CommandLine {
    std::vector<std::string> args;
    std::vector<char*> argv;
    explicit CommandLine(std::vector<std::string> given) : args(std::move(given)) {
        args.insert(args.begin(), "opensupcom");
        argv.reserve(args.size());
        for (auto& a : args) argv.push_back(a.data());
    }
    int argc() const { return static_cast<int>(argv.size()); }
};

} // namespace

TEST_CASE("A value option given last is refused", "[app][cli]") {
    // The option without its value, named, and no options: it was read as
    // not given (a last --ticks made a headless run a windowed one, a last
    // --faf-data played another install than the one asked for)
    const auto parse = [](std::vector<std::string> args) {
        CommandLine line(std::move(args));
        const char* missing = osc::app::option_without_value(line.argc(), line.argv.data());
        const bool parsed = osc::app::parse_options(line.argc(), line.argv.data(), {}).has_value();
        return std::pair{std::string(missing ? missing : ""), parsed};
    };
    const std::string map = "/maps/SCMP_009/SCMP_009_scenario.lua";
    for (const char* option : {"--ticks", "--init", "--fa-path", "--faf-data", "--map", "--click",
                               "--click-at", "--mouse", "--load", "--save", "--seed", "--log",
                               "--user-dir", "--screenshot", "--gpgnet", "--count-marks"}) {
        CAPTURE(option);
        const auto [missing, parsed] = parse({"--map", map, option});
        CHECK(missing == option);
        CHECK_FALSE(parsed);
    }
    const std::pair<std::string, bool> passes{"", true};
    // Given their values, they pass
    CHECK(parse({"--map", map, "--ticks", "5"}) == passes);
    CHECK(parse({"--faf-data", "", "--map", map}) == passes); // "" is "not given"
    // The argument after an option is its value, whatever it says
    CHECK(parse({"--ticks", "5", "--map", map}) == passes);
    // Moho's own options pass over a missing value, as Moho's command line
    // does; --collect-logs's zip is optional; a flag takes nothing
    CHECK(parse({"--map", map, "/gpgnet"}) == passes);
    CHECK(parse({"--collect-logs"}) == passes);
    CHECK(parse({"--map", map, "--no-fog"}) == passes);
}

TEST_CASE("Every option the help gives a value takes one", "[app][cli]") {
    // Each "--name <value>" of --help is refused given last, so a new one
    // must be listed with the others
    std::ostringstream help;
    auto* const was = std::cout.rdbuf(help.rdbuf());
    osc::app::print_usage();
    std::cout.rdbuf(was);
    const std::string text = help.str();
    const std::regex value_option(R"((--[a-z][a-z-]*) [<"])");
    int found = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), value_option), end; it != end; ++it) {
        const std::string name = (*it)[1];
        CAPTURE(name);
        CommandLine line({name});
        CHECK(osc::app::option_without_value(line.argc(), line.argv.data()) != nullptr);
        ++found;
    }
    CHECK(found >= 40);
}
