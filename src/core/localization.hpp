#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::core {

/// Caches localization strings from /loc/us/strings_db.lua.
/// LOC(key) does a single hash lookup — no Lua pcall per invocation.
/// FA convention: if key not found, return the key itself.
class Localization {
public:
    /// Add a single key→string mapping.
    void add(const std::string& key, const std::string& value);

    /// Load all strings from /loc/us/strings_db.lua via VFS + Lua.
    void load_from_vfs(lua_State* L, vfs::VirtualFileSystem* vfs);

    /// Look up a localization key. Returns the value, or key itself if missing.
    /// Handles FA's "<LOC key>" wrapper format.
    const std::string& lookup(const std::string& key) const;

    /// LOCF: look up key, then substitute %s/%d args positionally.
    std::string format(const std::string& key,
                       const std::vector<std::string>& args) const;

    size_t size() const { return strings_.size(); }

private:
    std::unordered_map<std::string, std::string> strings_;
    mutable std::string fallback_; // for returning ref to missing keys
};

} // namespace osc::core
