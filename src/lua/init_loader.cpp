#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/script_loader.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/blueprint_bindings.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "vfs/virtual_file_system.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/zip_mount.hpp"
#include "vfs/path_utils.hpp"
#include "blueprints/blueprint_store.hpp"

#include <algorithm>
#include <string>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

namespace {

/// The script that loads the game rules: the blueprints, and what they need.
constexpr const char* kRuleInit = "/lua/RuleInit.lua";

/// The strings in the init script's global array `name` (`hook`,
/// `protocols`), in order; empty when it sets none.
std::vector<std::string> string_array(lua_State* L, const char* name) {
    std::vector<std::string> out;
    lua_getglobal(L, name);
    if (lua_istable(L, -1)) {
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

} // namespace

Result<void> InitLoader::execute_init(LuaState& state,
                                        const InitConfig& config,
                                        vfs::VirtualFileSystem& vfs) {
    // The init file runs with Lua's io and os libraries (a new state opens
    // them), as in Moho's start-up state (LuaState::Init with LIB_OSIO): it
    // lists the player's folders and reads files there (FAF's init_faf.lua,
    // io.dir and io.open). It runs first in a new state, before any of the
    // game's scripts, so nothing untrusted has touched the state yet.
    //
    // The game's scripts then run in this state, and Moho's game states have
    // neither library (LIB_BASE: the rules', the sim's, the session UI's and
    // the front end's). Lua 5.0's luaopen_io opens both io and os, so both go
    // when this returns, whether the init succeeded or not (a failed reload
    // keeps its state): a mod's or a map's script can no longer write, delete
    // or list the player's files, nor run a command. The base library's
    // dofile, loadfile and require still read real paths, as Moho's do; none
    // of retail's or FAF's game scripts call them.
    class DropOsLibraries {
    public:
        explicit DropOsLibraries(lua_State* L) : L_(L) {}
        DropOsLibraries(const DropOsLibraries&) = delete;
        DropOsLibraries& operator=(const DropOsLibraries&) = delete;
        DropOsLibraries(DropOsLibraries&&) = delete;
        DropOsLibraries& operator=(DropOsLibraries&&) = delete;
        ~DropOsLibraries() {
            for (const char* name : {"io", "os"}) {
                lua_pushstring(L_, name);
                lua_pushnil(L_);
                lua_rawset(L_, LUA_GLOBALSINDEX);
            }
        }

    private:
        lua_State* L_;
    };
    const DropOsLibraries drop_os_libraries(state.raw());

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
    state.set_global_string("ClientVersion",
                            (std::string("OpenSupCom ") + core::version()).c_str());
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
    if (auto r = build_vfs_from_path_table(state.raw(), vfs); !r) return r;
    read_hook_table(state.raw(), vfs);
    url_protocols_ = string_array(state.raw(), "protocols");
    return {};
}

namespace {

/// Mount a single resolved path: archives as ZipMount, directories as
/// DirectoryMount. Returns false (and logs) for anything else.
bool mount_one(vfs::VirtualFileSystem& vfs, const fs::path& path,
               const std::string& mountpoint) {
    std::string ext_lower = path.extension().string();
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::error_code ec;
    if (ext_lower == ".scd" || ext_lower == ".nx2" || ext_lower == ".nx5" ||
        ext_lower == ".zip") {
        if (fs::is_regular_file(path, ec)) {
            vfs.mount(mountpoint, std::make_unique<vfs::ZipMount>(path));
            return true;
        }
    }
    if (fs::is_directory(path, ec)) {
        vfs.mount(mountpoint, std::make_unique<vfs::DirectoryMount>(path));
        return true;
    }
    spdlog::warn("Skipping unknown mount type: {}", path.string());
    return false;
}

} // namespace

void InitLoader::read_hook_table(lua_State* L, vfs::VirtualFileSystem& vfs) {
    vfs.set_hook_dirs(string_array(L, "hook"));
    for (const auto& dir : vfs.hook_dirs()) {
        spdlog::info("VFS hook directory: {}", dir);
    }
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
            std::replace(dir_str.begin(), dir_str.end(), '\\', '/');

            // Entries may be globs (retail SupComDataPath.lua mounts
            // "<bin>\\..\\gamedata\\*.scd"), and paths written on Windows
            // may not match on-disk case; expand_glob handles both.
            auto matches = vfs::expand_glob(fs::path(dir_str));
            if (matches.empty()) {
                spdlog::debug("Mount path does not exist, skipping: {}",
                              dir_str);
            }
            for (const auto& dir_path : matches) {
                if (mount_one(vfs, dir_path, mountpoint)) count++;
            }
        }

        lua_pop(L, 3); // pop dir, mountpoint, entry table
    }

    lua_pop(L, 1); // pop path table

    spdlog::info("VFS: {} mount points active", count);
    return {};
}

Result<void> run_system_scripts(LuaState& state, const vfs::VirtualFileSystem& vfs) {
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
        // The footprint classes, before the blueprints (RuleInit.lua's order).
        "/lua/footprints.lua",
        "/lua/system/Blueprints.lua",
    };

    for (const char* file : system_files) {
        if (!vfs.file_exists(file)) {
            spdlog::warn("System file not found in VFS: {}", file);
            continue;
        }

        auto result = run_vfs_script(state.raw(), file);
        if (!result) {
            spdlog::error("Failed to load {}: {}", file,
                          result.error().message);
            return Error("Failed to load system file " + std::string(file) +
                         ": " + result.error().message);
        }
        spdlog::info("  Loaded: {}", file);
    }
    return {};
}

Result<void> InitLoader::load_blueprints(
    LuaState& state, const vfs::VirtualFileSystem& vfs,
    blueprints::BlueprintStore& store) {
    // Store VFS and BlueprintStore pointers for C bindings to access
    state.set_vfs(const_cast<vfs::VirtualFileSystem*>(&vfs));
    state.set_blueprint_store(&store);
    store.set_file_exists([&vfs](const std::string& path) { return vfs.file_exists(path); });

    // Register blueprint-context bindings
    register_blueprint_bindings(state);
    register_blueprint_store_bindings(state);

    // Logging functions should already be registered from init phase,
    // but re-register just in case
    state.register_function("LOG", log::l_LOG);
    state.register_function("WARN", log::l_WARN);
    state.register_function("SPEW", log::l_SPEW);
    state.register_function("_ALERT", log::l_ALERT);

    // The rules load, as Moho's RRuleGameRulesImpl constructor runs it
    // (FUN_00529120): __active_mods is already the game's (the launch set
    // it, or register_blueprint_bindings gave an empty one), then
    // /lua/RuleInit.lua runs as doscript runs a script, so the init hook
    // directories' and each active mod's hooks of it run too. The script
    // loads the system scripts the blueprints need (config, repr and utils;
    // FAF's also debug.lua), logs the game's mods, loads /lua/footprints.lua
    // and Blueprints.lua, and calls LoadBlueprints().
    if (!vfs.file_exists(kRuleInit)) {
        return Error(std::string(kRuleInit) + " not found: the data has no blueprints to load");
    }
    spdlog::info("Running {}...", kRuleInit);
    // Moho's SCR_LuaDoScript reports an error in it (or in a mod's hook of
    // it), and the load goes on with what was registered: a hook runs after
    // LoadBlueprints(), so a broken one costs the game no blueprints. A hook
    // that doesn't compile stops the whole chunk, as in Moho; with no
    // blueprints there is no game to play, so that one fails the load.
    if (auto result = run_vfs_script(state.raw(), kRuleInit); !result) {
        spdlog::error("Error in file {} : {}", kRuleInit, result.error().message);
        if (store.registered_count() == 0) {
            return Error(std::string(kRuleInit) +
                         " loaded no blueprints: " + result.error().message);
        }
    }

    // The engine's own: this state goes on to be the sim's (or a game UI's),
    // and the engine imports modules into it before simInit.lua (or
    // userInit.lua) loads the class system and import() again -- the
    // scenario loader's /lua/terraintypes.lua, for one. Moho's rules state
    // has neither (its import() is a stub that answers false), so they load
    // after RuleInit.lua, where no blueprint or hook of it can use them.
    for (const char* file : {"/lua/system/class.lua", "/lua/system/import.lua"}) {
        if (!vfs.file_exists(file)) {
            spdlog::warn("System file not found in VFS: {}", file);
            continue;
        }
        if (auto result = run_vfs_script(state.raw(), file); !result) {
            spdlog::error("Failed to load {}: {}", file, result.error().message);
            return Error("Failed to load system file " + std::string(file) + ": " +
                         result.error().message);
        }
    }

    // Expose __blueprints global to Lua (needed by shield.lua, game.lua, etc.)
    store.copy_lua_tables(state.raw());
    store.expose_to_lua(state.raw());

    // Log results
    store.log_statistics();

    return {};
}

} // namespace osc::lua
