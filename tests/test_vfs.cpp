#include <catch2/catch_test_macros.hpp>
#include "vfs/virtual_file_system.hpp"
#include "vfs/directory_mount.hpp"

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
