#include <catch2/catch_test_macros.hpp>

#include "platform/executable.hpp"
#include "platform/paths.hpp"
#include "support/temp_path.hpp"

#include <filesystem>
#include <map>
#include <string>

using namespace osc::platform;

namespace {

/// Build an EnvLookup over a fixed map, so tests never read the real env.
EnvLookup fake_env(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* key) -> std::optional<std::string> {
        auto it = vars.find(key);
        if (it == vars.end()) return std::nullopt;
        return it->second;
    };
}

} // namespace

#ifndef _WIN32

TEST_CASE("known_folder uses XDG variables when set", "[platform]") {
    auto env = fake_env({{"HOME", "/home/u"},
                         {"XDG_DATA_HOME", "/data"},
                         {"XDG_CONFIG_HOME", "/cfg"},
                         {"XDG_CACHE_HOME", "/cache"},
                         {"XDG_STATE_HOME", "/state"},
                         {"XDG_DOCUMENTS_DIR", "/docs"}});
    CHECK(known_folder(KnownFolder::LocalAppData, env) == "/data");
    CHECK(known_folder(KnownFolder::Config, env) == "/cfg");
    CHECK(known_folder(KnownFolder::Cache, env) == "/cache");
    CHECK(known_folder(KnownFolder::State, env) == "/state");
    CHECK(known_folder(KnownFolder::Documents, env) == "/docs");
}

#ifdef __APPLE__

TEST_CASE("known_folder falls back to ~/Library on macOS", "[platform]") {
    auto env = fake_env({{"HOME", "/home/u"}});
    CHECK(known_folder(KnownFolder::LocalAppData, env) == "/home/u/Library/Application Support");
    CHECK(known_folder(KnownFolder::Config, env) == "/home/u/Library/Application Support");
    CHECK(known_folder(KnownFolder::Cache, env) == "/home/u/Library/Caches");
    CHECK(known_folder(KnownFolder::State, env) == "/home/u/Library/Application Support");
    CHECK(known_folder(KnownFolder::Documents, env) == "/home/u/Documents");

    // An earlier build's ~/.config/opensupcom is kept
    const osc::test::TempDir home("osc_paths_home");
    std::filesystem::create_directories(home.path() / ".config" / "opensupcom");
    auto legacy = fake_env({{"HOME", home.path().string()}});
    CHECK(known_folder(KnownFolder::Config, legacy) == home.path() / ".config");
    CHECK(known_folder(KnownFolder::State, legacy) ==
          home.path() / "Library" / "Application Support");
}

TEST_CASE("known_folder ignores relative and empty XDG values", "[platform]") {
    auto env =
        fake_env({{"HOME", "/home/u"}, {"XDG_CONFIG_HOME", "relative/dir"}, {"XDG_DATA_HOME", ""}});
    CHECK(known_folder(KnownFolder::Config, env) == "/home/u/Library/Application Support");
    CHECK(known_folder(KnownFolder::LocalAppData, env) == "/home/u/Library/Application Support");
}

#else

TEST_CASE("known_folder falls back to HOME-relative XDG defaults", "[platform]") {
    auto env = fake_env({{"HOME", "/home/u"}});
    CHECK(known_folder(KnownFolder::LocalAppData, env) == "/home/u/.local/share");
    CHECK(known_folder(KnownFolder::Config, env) == "/home/u/.config");
    CHECK(known_folder(KnownFolder::Cache, env) == "/home/u/.cache");
    CHECK(known_folder(KnownFolder::State, env) == "/home/u/.local/state");
    CHECK(known_folder(KnownFolder::Documents, env) == "/home/u/Documents");
}

TEST_CASE("known_folder ignores relative and empty XDG values", "[platform]") {
    // XDG Base Directory spec: relative paths are invalid and must be ignored.
    auto env = fake_env({{"HOME", "/home/u"},
                         {"XDG_CONFIG_HOME", "relative/dir"},
                         {"XDG_DATA_HOME", ""}});
    CHECK(known_folder(KnownFolder::Config, env) == "/home/u/.config");
    CHECK(known_folder(KnownFolder::LocalAppData, env) == "/home/u/.local/share");
}

#endif

TEST_CASE("home_dir falls back when HOME is unset", "[platform]") {
    auto env = fake_env({});
    auto home = home_dir(env);
    CHECK_FALSE(home.empty());
    CHECK(home.is_absolute());
}

#endif

TEST_CASE("known_folder with the real environment returns absolute paths",
          "[platform]") {
    for (auto folder : {KnownFolder::Documents, KnownFolder::LocalAppData,
                        KnownFolder::Config, KnownFolder::Cache,
                        KnownFolder::State}) {
        auto p = known_folder(folder);
        CHECK(p.is_absolute());
    }
}

TEST_CASE("The running executable is found, and on macOS its linker's UUID", "[platform]") {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    const std::filesystem::path exe = executable_path();
    REQUIRE_FALSE(exe.empty());
    CHECK(exe.is_absolute());
    CHECK(std::filesystem::is_regular_file(exe));
#endif
#ifdef __APPLE__
    CHECK(executable_build_id().size() == 16); // LC_UUID
#endif
}
