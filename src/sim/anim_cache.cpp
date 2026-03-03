#include "sim/anim_cache.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

namespace osc::sim {

AnimCache::AnimCache(vfs::VirtualFileSystem* vfs) : vfs_(vfs) {}

const SCAData* AnimCache::get(const std::string& vfs_path) {
    // Check cache
    auto it = cache_.find(vfs_path);
    if (it != cache_.end()) return it->second.get();

    // Check failed set (don't retry)
    if (failed_.count(vfs_path)) return nullptr;

    // Read .sca file from VFS
    auto file_data = vfs_->read_file(vfs_path);
    if (!file_data) {
        spdlog::debug("AnimCache: VFS read failed for '{}'", vfs_path);
        failed_.insert(vfs_path);
        return nullptr;
    }

    // Parse SCA
    auto sca = parse_sca(*file_data);
    if (!sca) {
        spdlog::debug("AnimCache: SCA parse failed for '{}'", vfs_path);
        failed_.insert(vfs_path);
        return nullptr;
    }

    spdlog::debug("AnimCache: loaded {} frames, {} bones from '{}'",
                   sca->num_frames, sca->num_bones, vfs_path);

    auto ptr = std::make_unique<SCAData>(std::move(*sca));
    auto* raw = ptr.get();
    cache_[vfs_path] = std::move(ptr);
    return raw;
}

} // namespace osc::sim
