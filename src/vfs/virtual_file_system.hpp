#pragma once

#include "vfs/mount_point.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace osc::vfs {

/// Overlay virtual file system. Mounts are searched in order
/// (first-mounted-wins) to match the original engine's patching semantics:
/// FAF mounts patch .nx2 files first, then base .scd files.
class VirtualFileSystem {
public:
    /// Add a mount. Earlier mounts have higher priority.
    void mount(std::string mountpoint, std::unique_ptr<MountPoint> source);

    /// Read a file from the VFS.
    std::optional<std::vector<char>> read_file(std::string_view path) const;

    /// Check if a file exists in the VFS.
    bool file_exists(std::string_view path) const;

    /// Find all files matching a pattern under a directory.
    std::vector<std::string> find_files(
        std::string_view directory, std::string_view pattern) const;

    /// Get file info from the VFS.
    std::optional<FileInfo> get_file_info(std::string_view path) const;

    /// Number of active mounts.
    size_t mount_count() const { return mounts_.size(); }

    /// Clear all mounts.
    void clear();

    /// Normalize a virtual path: lowercase, forward slashes, collapse .. and .
    static std::string normalize(std::string_view path);

private:
    struct MountEntry {
        std::string mountpoint; // Normalized, e.g., "/" or "/maps/mymap"
        std::unique_ptr<MountPoint> source;
    };

    std::vector<MountEntry> mounts_;

    /// Try to strip the mountpoint prefix from a path.
    /// Returns the remainder if path starts with mountpoint, nullopt otherwise.
    static std::optional<std::string> strip_mountpoint(
        std::string_view path, std::string_view mountpoint);
};

} // namespace osc::vfs
