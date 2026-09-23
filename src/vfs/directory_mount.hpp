#pragma once

#include "vfs/mount_point.hpp"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace osc::vfs {

/// Mount point backed by a real filesystem directory.
///
/// Virtual paths arrive lowercased (VirtualFileSystem::normalize), while FA's
/// on-disk data keeps its Windows casing ("maps/SCMP_009/..."). On
/// case-sensitive filesystems the mount therefore keeps a lazily built index
/// from lowercase relative path to real relative path.
class DirectoryMount : public MountPoint {
public:
    explicit DirectoryMount(std::filesystem::path root);

    std::optional<std::vector<char>> read_file(
        std::string_view relative_path) const override;
    bool file_exists(std::string_view relative_path) const override;
    std::vector<std::string> find_files(
        std::string_view directory,
        std::string_view pattern) const override;
    std::optional<FileInfo> get_file_info(
        std::string_view relative_path) const override;

private:
    std::filesystem::path root_;

    /// lowercase generic relative path ("maps/scmp_009") -> real relative path.
    /// Built once on first lookup; read-only afterwards, so concurrent readers
    /// (async texture loading) need no further locking.
    mutable std::once_flag index_once_;
    mutable std::unordered_map<std::string, std::filesystem::path> index_;

    void build_index() const;

    /// Resolve a virtual relative path to an actual filesystem path, matching
    /// case-insensitively via the index. On an index miss only the exact
    /// spelling is tried (one stat): this mount often sits last in the VFS
    /// search order and sees every miss, so no directory walk happens here.
    /// Consequently a mixed-case file created after the index was built is
    /// invisible until the mount is recreated.
    std::filesystem::path resolve(std::string_view relative_path) const;
};

} // namespace osc::vfs
