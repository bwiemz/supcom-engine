#pragma once

// In-memory VFS mount for tests: files are added by virtual path and served
// back through the MountPoint interface, so Lua/VFS code can be exercised
// without game data or a real filesystem.

#include "vfs/mount_point.hpp"
#include "vfs/path_utils.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osc::test {

class MemoryMount final : public osc::vfs::MountPoint {
public:
    void add(const std::string& path, std::vector<char> data) {
        files_[osc::vfs::VirtualFileSystem::normalize(path)] = std::move(data);
    }

    void add(const std::string& path, std::string_view text) {
        add(path, std::vector<char>(text.begin(), text.end()));
    }

    std::optional<std::vector<char>> read_file(
        std::string_view relative_path) const override {
        auto path = osc::vfs::VirtualFileSystem::normalize(relative_path);
        auto it = files_.find(path);
        if (it == files_.end()) return std::nullopt;
        return it->second;
    }

    bool file_exists(std::string_view relative_path) const override {
        auto path = osc::vfs::VirtualFileSystem::normalize(relative_path);
        return files_.find(path) != files_.end();
    }

    /// The files under `directory` whose names match `pattern`, sorted, as
    /// the real mounts list them (script hooks are found by a listing).
    std::vector<std::string> find_files(std::string_view directory,
                                        std::string_view pattern) const override {
        std::string dir = osc::vfs::VirtualFileSystem::normalize(directory);
        if (dir.back() != '/') dir += '/';
        std::vector<std::string> found;
        for (const auto& [path, data] : files_) {
            if (path.compare(0, dir.size(), dir) != 0) continue;
            const std::string_view name = std::string_view(path).substr(path.rfind('/') + 1);
            if (osc::vfs::wildcard_match(pattern, name)) found.push_back(path);
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    std::optional<osc::vfs::FileInfo> get_file_info(
        std::string_view relative_path) const override {
        auto path = osc::vfs::VirtualFileSystem::normalize(relative_path);
        auto it = files_.find(path);
        if (it == files_.end()) return std::nullopt;
        osc::vfs::FileInfo info;
        info.size_bytes = static_cast<osc::u64>(it->second.size());
        return info;
    }

private:
    std::unordered_map<std::string, std::vector<char>> files_;
};

} // namespace osc::test
