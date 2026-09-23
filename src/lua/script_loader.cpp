#include "lua/script_loader.hpp"

#include "lua/lua_state.hpp"
#include "vfs/virtual_file_system.hpp"

#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// Load and run one chunk. On success the stack is unchanged; on failure the
/// error message is returned and the stack is unchanged.
Result<void> run_chunk(lua_State* L, const std::vector<char>& data,
                       const std::string& path, int env_index) {
    // Strip a UTF-8 BOM (e.g. loc/us/strings_db.lua).
    const char* buf = data.data();
    size_t len = data.size();
    if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf += 3;
        len -= 3;
    }

    const std::string chunk_name = "@" + path;
    if (luaL_loadbuffer(L, buf, len, chunk_name.c_str()) != 0) {
        std::string msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)";
        lua_pop(L, 1);
        return Error(msg);
    }
    if (env_index != 0) {
        lua_pushvalue(L, env_index);
        lua_setfenv(L, -2);
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        std::string msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)";
        lua_pop(L, 1);
        return Error(msg);
    }
    return {};
}

} // namespace

Result<void> run_vfs_script(lua_State* L, std::string_view path, int env_index) {
    auto* vfs = LuaState::get_vfs(L);
    if (!vfs) return Error("doscript: VFS not initialized");

    // Relative stack indices would shift as we push; pin it.
    if (env_index < 0 && env_index > LUA_REGISTRYINDEX) {
        env_index = lua_gettop(L) + env_index + 1;
    }

    std::string script(path);
    auto data = vfs->read_file(script);
    if (!data) return Error("doscript: file not found: " + script);
    if (auto r = run_chunk(L, *data, script, env_index); !r) return r;

    // Hooks extend the script they shadow: /schook/lua/x.lua after /lua/x.lua.
    const std::string normalized = vfs::VirtualFileSystem::normalize(script);
    for (const auto& hook_dir : vfs->hook_dirs()) {
        const std::string hook_path = hook_dir + normalized;
        auto hook_data = vfs->read_file(hook_path);
        if (!hook_data) continue;
        if (auto r = run_chunk(L, *hook_data, hook_path, env_index); !r) return r;
    }
    return {};
}

} // namespace osc::lua
