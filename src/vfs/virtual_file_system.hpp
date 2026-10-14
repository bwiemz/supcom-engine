#pragma once

#include "vfs/mount_point.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osc::vfs {

/// Overlay virtual file system. Mounts are searched in order
/// (first-mounted-wins) to match the original engine's patching semantics:
/// FAF mounts patch .nx2 files first, then base .scd files.
class VirtualFileSystem {
public:
    /// Add a mount. Earlier mounts have higher priority.
    void mount(const std::string& mountpoint, std::unique_ptr<MountPoint> source);

    /// Read a file from the VFS.
    std::optional<std::vector<char>> read_file(std::string_view path) const;

    /// Check if a file exists in the VFS.
    bool file_exists(std::string_view path) const;

    /// Find all files matching a pattern under a directory.
    std::vector<std::string> find_files(
        std::string_view directory, std::string_view pattern) const;

    /// Whether `path` is a file under `directory`, by a listing of the
    /// directory taken once (find_files) rather than a check of every mount.
    /// Script hooks ask it of each mod's hook folder for every script run:
    /// asked of the mounts, each check of a folder on disk went to the disk,
    /// which on Windows took most of a modded game's start. A new mount, or
    /// clear(), drops the listings.
    bool listed_file_exists(std::string_view directory, std::string_view path) const;

    /// Get file info from the VFS.
    std::optional<FileInfo> get_file_info(std::string_view path) const;

    /// Number of active mounts.
    size_t mount_count() const { return mounts_.size(); }

    /// Clear all mounts (hook directories are kept).
    void clear();

    /// Hook directories from the init script's `hook` table (retail and FAF
    /// both use {'/schook'}). When a script /a/b.lua is run through doscript,
    /// each <hook>/a/b.lua that exists joins its chunk, after it
    /// (lua::run_vfs_script). Stored normalised, in init-script order.
    void set_hook_dirs(const std::vector<std::string>& dirs);
    const std::vector<std::string>& hook_dirs() const { return hook_dirs_; }

    /// Normalize a virtual path: lowercase, forward slashes, collapse .. and .
    static std::string normalize(std::string_view path);

private:
    struct MountEntry {
        std::string mountpoint; // Normalized, e.g., "/" or "/maps/mymap"
        std::unique_ptr<MountPoint> source;
    };

    std::vector<MountEntry> mounts_;
    std::vector<std::string> hook_dirs_;

    /// listed_file_exists' listings, by normalised directory (scripts load
    /// on more than one thread).
    struct Listings {
        std::mutex mutex;
        std::unordered_map<std::string, std::unordered_set<std::string>> files;
    };
    std::unique_ptr<Listings> listings_ = std::make_unique<Listings>();
    void drop_listings();

    /// Whether `mountpoint` lies below `directory` (both normalised).
    static bool mounted_below(std::string_view mountpoint, std::string_view directory);

    /// Try to strip the mountpoint prefix from a path.
    /// Returns the remainder if path starts with mountpoint, nullopt otherwise.
    static std::optional<std::string> strip_mountpoint(
        std::string_view path, std::string_view mountpoint);
};

} // namespace osc::vfs
