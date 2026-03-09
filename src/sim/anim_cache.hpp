#pragma once

#include "sim/sca_parser.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::sim {

/// Lazy-loading animation cache keyed by VFS path.
/// Parses .sca files on first access and caches the result.
class AnimCache {
public:
    explicit AnimCache(vfs::VirtualFileSystem* vfs);

    /// Get parsed SCA data for a VFS path. Returns nullptr on failure.
    const SCAData* get(const std::string& vfs_path);

    /// Inject pre-built SCA data for testing (no VFS read).
    void inject(const std::string& path, SCAData data) {
        cache_[path] = std::make_unique<SCAData>(std::move(data));
    }

private:
    vfs::VirtualFileSystem* vfs_;
    std::unordered_map<std::string, std::unique_ptr<SCAData>> cache_;
    std::unordered_set<std::string> failed_;
};

} // namespace osc::sim
