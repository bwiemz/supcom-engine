#include "vfs/directory_mount.hpp"
#include "vfs/path_utils.hpp"

#include <algorithm>
#include <fstream>
#include <spdlog/spdlog.h>

namespace osc::vfs {

namespace {

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// "/Maps\\SCMP_009/" -> "maps/scmp_009"
std::string index_key(std::string_view relative_path) {
    std::string key = lowercase(std::string(relative_path));
    std::replace(key.begin(), key.end(), '\\', '/');
    while (!key.empty() && key.front() == '/') key.erase(0, 1);
    while (!key.empty() && key.back() == '/') key.pop_back();
    return key;
}

} // namespace

DirectoryMount::DirectoryMount(std::filesystem::path root)
    : root_(std::move(root)) {}

void DirectoryMount::build_index() const {
    std::error_code ec;
    size_t count = 0;
    for (std::filesystem::recursive_directory_iterator it(root_, ec), end;
         !ec && it != end; it.increment(ec)) {
        auto rel = it->path().lexically_relative(root_);
        // First spelling wins if two entries differ only by case.
        index_.try_emplace(lowercase(rel.generic_string()), rel);
        ++count;
    }
    spdlog::debug("DirectoryMount {}: indexed {} entries", root_.string(), count);
}

std::filesystem::path DirectoryMount::resolve(
    std::string_view relative_path) const {
    const std::string key = index_key(relative_path);
    if (key.empty()) return root_;

    std::call_once(index_once_, [this] { build_index(); });
    if (auto it = index_.find(key); it != index_.end()) {
        return root_ / it->second;
    }
    return root_ / std::filesystem::path(key);
}

std::optional<std::vector<char>> DirectoryMount::read_file(
    std::string_view relative_path) const {
    auto full_path = resolve(relative_path);

    std::ifstream file(full_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return std::nullopt;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(static_cast<size_t>(size));
    if (!file.read(buffer.data(), size)) {
        return std::nullopt;
    }

    return buffer;
}

bool DirectoryMount::file_exists(std::string_view relative_path) const {
    auto full_path = resolve(relative_path);
    return std::filesystem::exists(full_path);
}

std::vector<std::string> DirectoryMount::find_files(
    std::string_view directory, std::string_view pattern) const {
    auto dir_path = resolve(directory);
    std::vector<std::string> results;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir_path, ec)) {
        return results;
    }

    for (std::filesystem::recursive_directory_iterator it(dir_path, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) ||
            !wildcard_match(pattern, it->path().filename().string())) {
            continue;
        }
        // Virtual path relative to the mount root, lowercased like every
        // other VFS path.
        results.push_back(
            "/" + lowercase(it->path().lexically_relative(root_).generic_string()));
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::optional<FileInfo> DirectoryMount::get_file_info(
    std::string_view relative_path) const {
    auto full_path = resolve(relative_path);
    std::error_code ec;

    if (!std::filesystem::exists(full_path, ec)) {
        return std::nullopt;
    }

    FileInfo info;
    info.is_folder = std::filesystem::is_directory(full_path, ec);
    if (!info.is_folder) {
        info.size_bytes = std::filesystem::file_size(full_path, ec);
    }
    return info;
}

} // namespace osc::vfs
