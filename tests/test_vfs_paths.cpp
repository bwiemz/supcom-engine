#include <catch2/catch_test_macros.hpp>

#include "vfs/directory_mount.hpp"
#include "vfs/path_utils.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace osc::vfs;
namespace fs = std::filesystem;

namespace {

/// Self-deleting scratch directory under the system temp dir.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("osc_vfs_test_" + std::to_string(rd()) + std::to_string(rd()));
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

std::string to_string(const std::vector<char>& v) {
    return {v.begin(), v.end()};
}

} // namespace

TEST_CASE("wildcard_match is case-insensitive with * and ?", "[vfs][paths]") {
    CHECK(wildcard_match("*.scd", "lua.scd"));
    CHECK(wildcard_match("*.scd", "LOC_US.SCD"));
    CHECK(wildcard_match("*", "anything"));
    CHECK(wildcard_match("*.*", "a.b"));
    CHECK(wildcard_match("a?c", "ABC"));
    CHECK(wildcard_match("*_unit.bp", "uel0001_unit.bp"));
    CHECK(wildcard_match("*mid*", "xxMIDyy"));
    CHECK_FALSE(wildcard_match("*.scd", "lua.scd.bak"));
    CHECK_FALSE(wildcard_match("a?c", "ac"));
    CHECK_FALSE(wildcard_match("", "x"));
    CHECK(wildcard_match("", ""));
}

TEST_CASE("has_wildcard detects glob characters", "[vfs][paths]") {
    CHECK(has_wildcard("*.scd"));
    CHECK(has_wildcard("a?c"));
    CHECK_FALSE(has_wildcard("lua.scd"));
}

TEST_CASE("resolve_case_insensitive finds real on-disk casing", "[vfs][paths]") {
    TempDir tmp;
    write_file(tmp.path / "Maps" / "SCMP_009" / "SCMP_009_scenario.lua", "x");

    auto resolved = resolve_case_insensitive(tmp.path / "maps" / "scmp_009" /
                                             "scmp_009_SCENARIO.lua");
    REQUIRE(resolved.has_value());
    // On case-sensitive filesystems the on-disk spelling comes back; on
    // Windows the lowercase path already exists and is returned as given.
    CHECK(fs::equivalent(*resolved,
                         tmp.path / "Maps" / "SCMP_009" / "SCMP_009_scenario.lua"));

    // Exact paths resolve to themselves.
    auto exact = resolve_case_insensitive(tmp.path / "Maps");
    REQUIRE(exact.has_value());
    CHECK(*exact == tmp.path / "Maps");

    // '..' components are honoured.
    auto dotdot = resolve_case_insensitive(tmp.path / "maps" / ".." / "MAPS");
    REQUIRE(dotdot.has_value());
    CHECK(fs::equivalent(*dotdot, tmp.path / "Maps"));

    CHECK_FALSE(resolve_case_insensitive(tmp.path / "nope" / "x.lua").has_value());
}

TEST_CASE("expand_glob lists matches sorted case-insensitively", "[vfs][paths]") {
    TempDir tmp;
    write_file(tmp.path / "GameData" / "units.scd", "u");
    write_file(tmp.path / "GameData" / "Lua.scd", "l");
    write_file(tmp.path / "GameData" / "effects.SCD", "e");
    write_file(tmp.path / "GameData" / "readme.txt", "r");
    fs::create_directories(tmp.path / "GameData" / "dir.scd"); // directories match too

    auto matches = expand_glob(tmp.path / "gamedata" / "*.scd");
    REQUIRE(matches.size() == 4);
    CHECK(matches[0].filename() == "dir.scd");
    CHECK(matches[1].filename() == "effects.SCD");
    CHECK(matches[2].filename() == "Lua.scd");
    CHECK(matches[3].filename() == "units.scd");

    CHECK(expand_glob(tmp.path / "missing" / "*.scd").empty());
}

TEST_CASE("DirectoryMount resolves virtual paths case-insensitively", "[vfs][paths]") {
    TempDir tmp;
    write_file(tmp.path / "Maps" / "SCMP_009" / "SCMP_009_scenario.lua", "scenario");
    write_file(tmp.path / "Sounds" / "Music.xwb", "music");

    DirectoryMount mount(tmp.path);

    // VirtualFileSystem lowercases paths before they reach the mount.
    auto data = mount.read_file("/maps/scmp_009/scmp_009_scenario.lua");
    REQUIRE(data.has_value());
    CHECK(to_string(*data) == "scenario");

    CHECK(mount.file_exists("/sounds/music.xwb"));
    CHECK(mount.file_exists("/SOUNDS/MUSIC.XWB"));
    CHECK_FALSE(mount.file_exists("/sounds/missing.xwb"));

    auto info = mount.get_file_info("/maps/scmp_009");
    REQUIRE(info.has_value());
    CHECK(info->is_folder);

    auto found = mount.find_files("/maps", "*.lua");
    REQUIRE(found.size() == 1);
    CHECK(found[0] == "/maps/scmp_009/scmp_009_scenario.lua");
}

TEST_CASE("DirectoryMount sees exact-spelled files created after indexing",
          "[vfs][paths]") {
    TempDir tmp;
    write_file(tmp.path / "a.txt", "a");
    DirectoryMount mount(tmp.path);
    REQUIRE(mount.file_exists("/a.txt")); // forces the index to build

    write_file(tmp.path / "later" / "b.txt", "b");
    auto data = mount.read_file("/later/b.txt");
    REQUIRE(data.has_value());
    CHECK(to_string(*data) == "b");
}

TEST_CASE("VFS reads through a mixed-case directory mount", "[vfs][paths]") {
    TempDir tmp;
    write_file(tmp.path / "Lua" / "System" / "Config.lua", "cfg");

    VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<DirectoryMount>(tmp.path));

    auto data = vfs.read_file("/lua/system/config.lua");
    REQUIRE(data.has_value());
    CHECK(to_string(*data) == "cfg");
}
