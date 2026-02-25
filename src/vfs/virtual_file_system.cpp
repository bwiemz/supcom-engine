#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <set>
#include <spdlog/spdlog.h>

namespace osc::vfs {

std::string VirtualFileSystem::normalize(std::string_view path) {
    std::string result(path);

    // Forward slashes
    std::replace(result.begin(), result.end(), '\\', '/');

    // Lowercase
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Collapse // into /
    std::string collapsed;
    collapsed.reserve(result.size());
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '/' && !collapsed.empty() && collapsed.back() == '/') {
            continue;
        }
        collapsed += result[i];
    }
    result = collapsed;

    // Collapse /./
    while (true) {
        auto pos = result.find("/./");
        if (pos == std::string::npos) break;
        result.erase(pos, 2);
    }

    // Collapse /../
    while (true) {
        auto pos = result.find("/../");
        if (pos == std::string::npos) break;
        if (pos == 0) {
            result.erase(0, 3);
            continue;
        }
        auto parent = result.rfind('/', pos - 1);
        if (parent == std::string::npos) {
            result.erase(0, pos + 4);
        } else {
            result.erase(parent, pos - parent + 3);
        }
    }

    // Ensure leading slash
    if (result.empty() || result[0] != '/') {
        result = "/" + result;
    }

    // Remove trailing slash (unless it's just "/")
    if (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }

    return result;
}

std::optional<std::string> VirtualFileSystem::strip_mountpoint(
    std::string_view path, std::string_view mountpoint) {
    // Root mount matches everything
    if (mountpoint == "/") {
        return std::string(path);
    }

    // Check if path starts with mountpoint
    if (path.size() >= mountpoint.size() &&
        path.compare(0, mountpoint.size(), mountpoint) == 0) {
        // The character after mountpoint must be '/' or end of string
        if (path.size() == mountpoint.size()) {
            return std::string("/");
        }
        if (path[mountpoint.size()] == '/') {
            return std::string(path.substr(mountpoint.size()));
        }
    }

    return std::nullopt;
}

void VirtualFileSystem::mount(std::string mountpoint,
                               std::unique_ptr<MountPoint> source) {
    auto mp = normalize(mountpoint);
    spdlog::debug("VFS: mounting at '{}'", mp);
    mounts_.push_back({std::move(mp), std::move(source)});
}

std::optional<std::vector<char>> VirtualFileSystem::read_file(
    std::string_view path) const {
    auto norm = normalize(path);

    // Search mounts in order (first-mounted-wins) — FAF mounts patches
    // before base game content, so earlier mounts have higher priority.
    for (const auto& entry : mounts_) {
        auto remainder = strip_mountpoint(norm, entry.mountpoint);
        if (remainder) {
            auto data = entry.source->read_file(*remainder);
            if (data) return data;
        }
    }

    return std::nullopt;
}

bool VirtualFileSystem::file_exists(std::string_view path) const {
    auto norm = normalize(path);

    for (const auto& entry : mounts_) {
        auto remainder = strip_mountpoint(norm, entry.mountpoint);
        if (remainder && entry.source->file_exists(*remainder)) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> VirtualFileSystem::find_files(
    std::string_view directory, std::string_view pattern) const {
    auto norm_dir = normalize(directory);
    std::set<std::string> seen;
    std::vector<std::string> results;

    // Search all mounts; first-mounted entries take priority for dedup
    for (const auto& entry : mounts_) {
        auto remainder = strip_mountpoint(norm_dir, entry.mountpoint);
        if (!remainder) continue;

        auto found = entry.source->find_files(*remainder, pattern);
        for (auto& f : found) {
            // Reconstruct the full virtual path
            std::string full_path;
            if (entry.mountpoint == "/") {
                full_path = f;
            } else {
                full_path = normalize(entry.mountpoint + f);
            }

            if (!seen.contains(full_path)) {
                seen.insert(full_path);
                results.push_back(std::move(full_path));
            }
        }
    }

    return results;
}

std::optional<FileInfo> VirtualFileSystem::get_file_info(
    std::string_view path) const {
    auto norm = normalize(path);

    for (const auto& entry : mounts_) {
        auto remainder = strip_mountpoint(norm, entry.mountpoint);
        if (remainder) {
            auto info = entry.source->get_file_info(*remainder);
            if (info) return info;
        }
    }

    return std::nullopt;
}

void VirtualFileSystem::clear() {
    mounts_.clear();
}

} // namespace osc::vfs
