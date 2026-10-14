#include <catch2/catch_test_macros.hpp>
#include "vfs/virtual_file_system.hpp"
#include "vfs/directory_mount.hpp"
#include "support/temp_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace osc::vfs;

TEST_CASE("VFS path normalization", "[vfs]") {
    CHECK(VirtualFileSystem::normalize("/foo/bar") == "/foo/bar");
    CHECK(VirtualFileSystem::normalize("/foo//bar") == "/foo/bar");
    CHECK(VirtualFileSystem::normalize("/foo/./bar") == "/foo/bar");
    CHECK(VirtualFileSystem::normalize("/foo/../bar") == "/bar");
    CHECK(VirtualFileSystem::normalize("\\foo\\bar") == "/foo/bar");
    CHECK(VirtualFileSystem::normalize("/FOO/BAR") == "/foo/bar");
    CHECK(VirtualFileSystem::normalize("foo/bar") == "/foo/bar");
    CHECK(VirtualFileSystem::normalize("/") == "/");
}

TEST_CASE("VFS mount and read", "[vfs]") {
    // This test requires a temp directory — skip in CI if needed
    // For now, just test the VFS without actual mounts
    VirtualFileSystem vfs;
    CHECK(vfs.mount_count() == 0);
    CHECK_FALSE(vfs.file_exists("/nonexistent"));
    CHECK_FALSE(vfs.read_file("/nonexistent").has_value());
}

TEST_CASE("a directory mount never reads a directory as a file", "[vfs]") {
    // On Linux an ifstream opens a directory; its size query fails, and an
    // empty path (the mount root) once turned that into a SIZE_MAX alloc.
    namespace fs = std::filesystem;
    const fs::path root = osc::test::unique_temp_path("osc_vfs_dir_read_test");
    fs::create_directories(root / "sub");
    { std::ofstream(root / "sub" / "file.txt") << "abc"; }

    DirectoryMount mount(root);
    CHECK_FALSE(mount.read_file("").has_value());
    CHECK_FALSE(mount.read_file("/").has_value());
    CHECK_FALSE(mount.read_file("/sub").has_value());
    auto data = mount.read_file("/sub/file.txt");
    REQUIRE(data.has_value());
    CHECK(std::string(data->begin(), data->end()) == "abc");
    fs::remove_all(root);
}

TEST_CASE("a listed folder answers as the mounts do, and a new mount lists it again", "[vfs]") {
    // Script hooks look each mod's hook folder up in a listing taken once,
    // not with a check of every mount per script.
    namespace fs = std::filesystem;
    const fs::path a = osc::test::unique_temp_path("osc_vfs_listing_a");
    const fs::path b = osc::test::unique_temp_path("osc_vfs_listing_b");
    fs::create_directories(a / "hook" / "lua" / "sim");
    fs::create_directories(b / "hook" / "lua");
    { std::ofstream(a / "hook" / "lua" / "sim" / "Unit.lua") << "-- a"; }
    { std::ofstream(b / "hook" / "lua" / "aibrain.lua") << "-- b"; }

    VirtualFileSystem vfs;
    vfs.mount("/mods/a", std::make_unique<DirectoryMount>(a));
    // Any spelling: paths are normalised (lowercase, forward slashes).
    CHECK(vfs.listed_file_exists("/mods/a/hook", "/mods/a/hook/lua/sim/unit.lua"));
    CHECK(vfs.listed_file_exists("/MODS/A/hook/", "\\mods\\a\\hook\\lua\\sim\\Unit.lua"));
    CHECK_FALSE(vfs.listed_file_exists("/mods/a/hook", "/mods/a/hook/lua/sim/weapon.lua"));
    // A folder is no file
    CHECK_FALSE(vfs.listed_file_exists("/mods/a/hook", "/mods/a/hook/lua/sim"));
    CHECK_FALSE(vfs.listed_file_exists("/mods/b/hook", "/mods/b/hook/lua/aibrain.lua"));

    // Mounting b drops the listings: b's folder is listed anew.
    vfs.mount("/mods/b", std::make_unique<DirectoryMount>(b));
    CHECK(vfs.listed_file_exists("/mods/b/hook", "/mods/b/hook/lua/aibrain.lua"));
    CHECK(vfs.listed_file_exists("/mods/a/hook", "/mods/a/hook/lua/sim/unit.lua"));

    vfs.clear();
    CHECK_FALSE(vfs.listed_file_exists("/mods/a/hook", "/mods/a/hook/lua/sim/unit.lua"));
    fs::remove_all(a);
    fs::remove_all(b);
}
