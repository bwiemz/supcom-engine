#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/blueprint_bindings.hpp"
#include "core/log.hpp"
#include "vfs/virtual_file_system.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/zip_mount.hpp"
#include "blueprints/blueprint_store.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

Result<void> InitLoader::execute_init(LuaState& state,
                                        const InitConfig& config,
                                        vfs::VirtualFileSystem& vfs) {
    // Register init-context bindings
    register_init_bindings(state);

    // Set globals that init.lua expects
    auto init_dir = config.init_file.parent_path().string();
    std::replace(init_dir.begin(), init_dir.end(), '\\', '/');
    state.set_global_string("InitFileDir", init_dir.c_str());

    // Set fa_path before executing init — init_faf.lua does
    // `dofile(InitFileDir .. '/../fa_path.lua')` but we provide it directly.
    auto fa_path_str = config.fa_path.string();
    std::replace(fa_path_str.begin(), fa_path_str.end(), '\\', '/');
    state.set_global_string("fa_path", fa_path_str.c_str());

    // Version globals
    state.set_global_string("ClientVersion", "OpenSupCom 0.1.0");
    state.set_global_string("GameVersion", "3831");
    state.set_global_string("GameType", "faf");

    // Custom vault path (optional)
    if (!config.faf_data_path.empty()) {
        auto vault_path = config.faf_data_path.string();
        std::replace(vault_path.begin(), vault_path.end(), '\\', '/');
        state.set_global_string("custom_vault_path", vault_path.c_str());
    }

    spdlog::info("Executing init file: {}", config.init_file.string());

    // Execute the init file
    auto result = state.do_file(config.init_file);
    if (!result) {
        return Error("Failed to execute init file: " + result.error().message);
    }

    // Build VFS from the path table
    return build_vfs_from_path_table(state.raw(), vfs);
}

Result<void> InitLoader::build_vfs_from_path_table(
    lua_State* L, vfs::VirtualFileSystem& vfs) {
    lua_getglobal(L, "path");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return Error("'path' global is not a table after init.lua execution");
    }

    int count = 0;
    // Iterate the array: path[1], path[2], ...
    for (int i = 1;; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }

        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        // Read dir field
        lua_pushstring(L, "dir");
        lua_gettable(L, -2);
        const char* dir = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;

        // Read mountpoint field
        lua_pushstring(L, "mountpoint");
        lua_gettable(L, -3);
        const char* mountpoint =
            lua_isstring(L, -1) ? lua_tostring(L, -1) : "/";

        if (dir) {
            std::string dir_str(dir);
            std::string mp_str(mountpoint);

            // Normalize slashes
            std::replace(dir_str.begin(), dir_str.end(), '\\', '/');

            fs::path dir_path(dir_str);
            std::error_code ec;

            if (fs::exists(dir_path, ec)) {
                // Determine if it's a ZIP file or directory
                std::string ext_lower = dir_path.extension().string();
                std::transform(ext_lower.begin(), ext_lower.end(),
                               ext_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (ext_lower == ".scd" || ext_lower == ".nx2" ||
                    ext_lower == ".nx5" || ext_lower == ".zip") {
                    vfs.mount(mp_str,
                              std::make_unique<vfs::ZipMount>(dir_path));
                } else if (fs::is_directory(dir_path, ec)) {
                    vfs.mount(mp_str,
                              std::make_unique<vfs::DirectoryMount>(dir_path));
                } else {
                    spdlog::warn("Skipping unknown mount type: {}", dir_str);
                }
                count++;
            } else {
                spdlog::debug("Mount path does not exist, skipping: {}",
                              dir_str);
            }
        }

        lua_pop(L, 3); // pop dir, mountpoint, entry table
    }

    lua_pop(L, 1); // pop path table

    spdlog::info("VFS: {} mount points active", count);
    return {};
}

Result<void> InitLoader::load_blueprints(
    LuaState& state, const vfs::VirtualFileSystem& vfs,
    blueprints::BlueprintStore& store) {
    // Store VFS and BlueprintStore pointers for C bindings to access
    state.set_vfs(const_cast<vfs::VirtualFileSystem*>(&vfs));
    state.set_blueprint_store(&store);

    // Register blueprint-context bindings
    register_blueprint_bindings(state);
    register_blueprint_store_bindings(state);

    // Logging functions should already be registered from init phase,
    // but re-register just in case
    state.register_function("LOG", log::l_LOG);
    state.register_function("WARN", log::l_WARN);
    state.register_function("SPEW", log::l_SPEW);
    state.register_function("_ALERT", log::l_ALERT);

    spdlog::info("Loading system Lua files...");

    // Pre-register Lua 5.1 compat shims needed before utils.lua loads.
    // utils.lua defines string.match/gmatch polyfills, but repr.lua loads
    // first and needs string.match. Pre-register them here.
    state.do_string(R"(
        rawset(string, 'match', function(input, exp, init)
            local match
            string.gsub(input:sub(init or 1), exp, function(...) match = arg end, 1)
            if match then return unpack(match) end
        end)
        rawset(string, 'gmatch', string.gfind)
    )");

    // Load system Lua files in dependency order.
    // These must be loaded via doscript from the VFS.
    const char* system_files[] = {
        "/lua/system/repr.lua",
        "/lua/system/config.lua",
        "/lua/system/utils.lua",
        "/lua/system/class.lua",
        "/lua/system/import.lua",
        "/lua/system/Blueprints.lua",
    };

    for (const char* file : system_files) {
        auto data = vfs.read_file(file);
        if (!data) {
            spdlog::warn("System file not found in VFS: {}", file);
            continue;
        }

        std::string chunk_name = std::string("@") + file;
        auto result =
            state.do_buffer(data->data(), data->size(), chunk_name.c_str());
        if (!result) {
            spdlog::error("Failed to load {}: {}", file,
                          result.error().message);
            return Error("Failed to load system file " + std::string(file) +
                         ": " + result.error().message);
        }
        spdlog::info("  Loaded: {}", file);
    }

    // Call LoadBlueprints() from a Lua chunk (not directly from C)
    // so that debug.getinfo() in GetSource() finds a Lua frame.
    spdlog::info("Calling LoadBlueprints()...");

    auto bp_result = state.do_string("LoadBlueprints()");
    if (!bp_result) {
        spdlog::error("LoadBlueprints() failed: {}",
                       bp_result.error().message);
        return Error("LoadBlueprints() failed: " +
                     bp_result.error().message);
    }

    // Expose __blueprints global to Lua (needed by shield.lua, game.lua, etc.)
    store.expose_to_lua(state.raw());

    // Log results
    store.log_statistics();

    return {};
}

} // namespace osc::lua
