#include <catch2/catch_test_macros.hpp>

#include "platform/game_install.hpp"
#include "platform/steam_library.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>

using namespace osc::platform;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("osc_install_test_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << contents;
}

EnvLookup fake_env(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* key) -> std::optional<std::string> {
        auto it = vars.find(key);
        if (it == vars.end()) return std::nullopt;
        return it->second;
    };
}

// Shaped like a real Linux Steam libraryfolders.vdf with two libraries.
std::string library_folders_vdf(const fs::path& lib0, const fs::path& lib1) {
    return "\"libraryfolders\"\n{\n"
           "\t\"0\"\n\t{\n"
           "\t\t\"path\"\t\t\"" + lib0.generic_string() + "\"\n"
           "\t\t\"label\"\t\t\"\"\n"
           "\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"191691814\"\n\t\t}\n"
           "\t}\n"
           "\t\"1\"\n\t{\n"
           "\t\t\"path\"\t\t\"" + lib1.generic_string() + "\"\n"
           "\t\t\"apps\"\n\t\t{\n\t\t\t\"9420\"\t\t\"8457249990\"\n\t\t}\n"
           "\t}\n"
           "}\n";
}

const char* kFaManifest =
    "\"AppState\"\n{\n"
    "\t\"appid\"\t\t\"9420\"\n"
    "\t\"name\"\t\t\"Supreme Commander: Forged Alliance\"\n"
    "\t\"installdir\"\t\t\"Supreme Commander Forged Alliance\"\n"
    "}\n";

/// A Steam root whose second library holds FA (with a retail init file).
fs::path make_steam_tree(const fs::path& base) {
    fs::path root = base / "home" / ".local" / "share" / "Steam";
    fs::path lib1 = base / "games" / "SteamLibrary";
    write_file(root / "steamapps" / "libraryfolders.vdf",
               library_folders_vdf(root, lib1));
    write_file(lib1 / "steamapps" / "appmanifest_9420.acf", kFaManifest);
    fs::path fa = lib1 / "steamapps" / "common" / "Supreme Commander Forged Alliance";
    write_file(fa / "bin" / "SupComDataPath.lua", "path = {}\n");
    write_file(fa / "gamedata" / "lua.scd", "");
    return fa;
}

} // namespace

TEST_CASE("VDF parser reads nested keys, escapes and comments", "[platform][install]") {
    auto root = parse_vdf(
        "// comment line\n"
        "\"outer\"\n{\n"
        "  \"plain\" \"value\"\n"
        "  \"win\" \"C:\\\\Program Files (x86)\\\\Steam\"\n"
        "  \"inner\" { \"k\" \"v\" }\n"
        "}\n");
    REQUIRE(root.has_value());
    const VdfNode* outer = root->child("outer");
    REQUIRE(outer != nullptr);
    CHECK(outer->value_of("plain") == "value");
    CHECK(outer->value_of("win") == "C:\\Program Files (x86)\\Steam");
    REQUIRE(outer->child("inner") != nullptr);
    CHECK(outer->child("inner")->value_of("k") == "v");
    CHECK(outer->value_of("missing").empty());
}

TEST_CASE("VDF parser rejects malformed input", "[platform][install]") {
    CHECK_FALSE(parse_vdf("\"a\" { \"b\" \"c\"").has_value());   // unclosed brace
    CHECK_FALSE(parse_vdf("\"a\" }").has_value());                // stray brace
    CHECK_FALSE(parse_vdf("\"unterminated").has_value());
}

TEST_CASE("VDF parser rejects pathologically deep nesting", "[platform][install]") {
    // Local but untrusted input: must fail cleanly, not overflow the stack.
    std::string deep;
    for (int i = 0; i < 100000; ++i) deep += "\"k\" { ";
    CHECK_FALSE(parse_vdf(deep).has_value());

    std::string ok_depth;
    for (int i = 0; i < 16; ++i) ok_depth += "\"k\" { ";
    for (int i = 0; i < 16; ++i) ok_depth += "} ";
    CHECK(parse_vdf(ok_depth).has_value());
}

TEST_CASE("libraryfolders.vdf yields every library path", "[platform][install]") {
    auto libs = parse_library_folders_vdf(
        library_folders_vdf("/home/u/.local/share/Steam", "/mnt/SteamLibrary"));
    REQUIRE(libs.size() == 2);
    CHECK(libs[0] == fs::path("/home/u/.local/share/Steam"));
    CHECK(libs[1] == fs::path("/mnt/SteamLibrary"));

    // Legacy (pre-2021) format: numbered keys map straight to paths.
    auto legacy = parse_library_folders_vdf(
        "\"LibraryFolders\"\n{\n \"TimeNextStatsReport\" \"1\"\n"
        " \"1\" \"D:\\\\SteamLibrary\"\n}\n");
    REQUIRE(legacy.size() == 1);
    CHECK(legacy[0] == fs::path("D:\\SteamLibrary"));
}

TEST_CASE("appmanifest installdir is extracted", "[platform][install]") {
    CHECK(parse_app_install_dir(kFaManifest) == "Supreme Commander Forged Alliance");
    CHECK_FALSE(parse_app_install_dir("\"AppState\" { }").has_value());
}

TEST_CASE("find_steam_app finds FA in a secondary library", "[platform][install]") {
    TempDir tmp;
    fs::path fa = make_steam_tree(tmp.path);
    auto found = find_steam_app(
        kForgedAllianceAppId, {tmp.path / "home" / ".local" / "share" / "Steam"});
    REQUIRE(found.has_value());
    CHECK(fs::equivalent(*found, fa));

    CHECK_FALSE(find_steam_app(12345, {tmp.path / "home" / ".local" / "share" / "Steam"})
                    .has_value());
    CHECK_FALSE(find_steam_app(kForgedAllianceAppId, {tmp.path / "nowhere"}).has_value());
}

TEST_CASE("fa_path.lua parser handles escaped Windows paths", "[platform][install]") {
    CHECK(parse_fa_path_lua("fa_path = \"C:\\\\Games\\\\FA\"\n") == fs::path("C:/Games/FA"));
    CHECK(parse_fa_path_lua("fa_path = \"/opt/fa\"") == fs::path("/opt/fa"));
    CHECK_FALSE(parse_fa_path_lua("-- nothing here").has_value());
}

#ifndef _WIN32

TEST_CASE("locate_game_install falls back to Steam retail FA", "[platform][install]") {
    TempDir tmp;
    fs::path fa = make_steam_tree(tmp.path);
    auto env = fake_env({{"HOME", (tmp.path / "home").string()}});

    auto result = locate_game_install({}, env);
    REQUIRE(result.install.has_value());
    CHECK(result.install->source == "steam");
    CHECK(fs::equivalent(result.install->fa_path, fa));
    CHECK(result.install->init_file.filename() == "SupComDataPath.lua");
    CHECK(result.install->faf_data_path.empty());
}

TEST_CASE("locate_game_install prefers FAF when its init exists", "[platform][install]") {
    TempDir tmp;
    fs::path fa = make_steam_tree(tmp.path);
    fs::path faf = tmp.path / "home" / ".faforever";
    write_file(faf / "bin" / "init_faf.lua", "-- faf\n");
    auto env = fake_env({{"HOME", (tmp.path / "home").string()}});

    auto result = locate_game_install({}, env);
    REQUIRE(result.install.has_value());
    CHECK(result.install->source == "faf");
    CHECK(result.install->init_file == faf / "bin" / "init_faf.lua");
    CHECK(result.install->faf_data_path == faf);
    // FAF has no fa_path.lua here, so FA itself comes from Steam.
    CHECK(fs::equivalent(result.install->fa_path, fa));
}

TEST_CASE("CLI hints and env override discovery", "[platform][install]") {
    TempDir tmp;
    make_steam_tree(tmp.path);
    fs::path custom = tmp.path / "custom_fa";
    write_file(custom / "bin" / "SupComDataPath.lua", "path = {}\n");
    auto env = fake_env({{"HOME", (tmp.path / "home").string()}});

    GameInstallHints hints;
    hints.fa_path = custom;
    auto from_cli = locate_game_install(hints, env);
    REQUIRE(from_cli.install.has_value());
    CHECK(from_cli.install->source == "cli");
    CHECK(from_cli.install->fa_path == custom);
    CHECK(from_cli.install->init_file == custom / "bin" / "SupComDataPath.lua");

    auto env2 = fake_env({{"HOME", (tmp.path / "home").string()},
                          {"OSC_FA_PATH", custom.string()}});
    auto from_env = locate_game_install({}, env2);
    REQUIRE(from_env.install.has_value());
    CHECK(from_env.install->source == "env");
    CHECK(from_env.install->fa_path == custom);
}

TEST_CASE("locate_game_install explains a failed search", "[platform][install]") {
    TempDir tmp;
    auto env = fake_env({{"HOME", (tmp.path / "empty_home").string()}});
    auto result = locate_game_install({}, env);
    CHECK_FALSE(result.install.has_value());
    CHECK_FALSE(result.searched.empty());
}

#endif
