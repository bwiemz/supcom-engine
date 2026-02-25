#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

extern "C" {
struct lua_State;
}

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::lua {

class LuaState;

/// Configuration for the init loader.
struct InitConfig {
    fs::path init_file;    ///< Path to init.lua / init_faf.lua
    fs::path fa_path;      ///< FA installation directory
    fs::path faf_data_path; ///< FAF data directory (parent of gamedata/)
};

/// Orchestrates the two-phase initialization:
/// Phase 1: Execute init.lua to discover VFS mount points.
/// Phase 2: Load blueprints using the VFS.
class InitLoader {
public:
    /// Phase 1: Execute the init file and build the VFS.
    Result<void> execute_init(LuaState& state, const InitConfig& config,
                              vfs::VirtualFileSystem& vfs);

    /// Phase 2: Load all blueprints via the VFS.
    Result<void> load_blueprints(LuaState& state,
                                  const vfs::VirtualFileSystem& vfs,
                                  blueprints::BlueprintStore& store);

private:
    /// Parse the path table from Lua state into VFS mounts.
    Result<void> build_vfs_from_path_table(lua_State* L,
                                            vfs::VirtualFileSystem& vfs);
};

} // namespace osc::lua
