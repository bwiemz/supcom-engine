#include <catch2/catch_test_macros.hpp>
#include "vfs/virtual_file_system.hpp"
#include "vfs/directory_mount.hpp"

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
    const fs::path root = fs::temp_directory_path() / "osc_vfs_dir_read_test";
    fs::remove_all(root);
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
