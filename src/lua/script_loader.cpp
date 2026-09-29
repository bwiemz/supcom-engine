#include "lua/script_loader.hpp"

#include "lua/lua_state.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// The error message on top of the stack, popped.
std::string pop_message(lua_State* L) {
    std::string msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)";
    lua_pop(L, 1);
    return msg;
}

/// Field `key` of the table on top of the stack, as LuaPlus's ToString
/// reads it (a number converts); "" when it is neither.
std::string string_field(lua_State* L, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, -2);
    std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return value;
}

/// Append a file's bytes to the chunk as Moho's concatenating reader does:
/// a newline follows a file that doesn't end in one. A UTF-8 BOM is dropped
/// (e.g. loc/us/strings_db.lua); an empty file adds nothing.
void append_file(std::string& chunk, const std::vector<char>& data) {
    const char* buf = data.data();
    size_t len = data.size();
    if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf += 3;
        len -= 3;
    }
    if (len == 0) return;
    chunk.append(buf, len);
    if (buf[len - 1] != '\n') chunk.push_back('\n');
}

} // namespace

std::vector<std::string> script_hooks(lua_State* L, const vfs::VirtualFileSystem& vfs,
                                      std::string_view path) {
    const std::string normalized = vfs::VirtualFileSystem::normalize(path);
    std::vector<std::string> hooks;
    auto take = [&](const std::string& candidate) {
        std::string hook = vfs::VirtualFileSystem::normalize(candidate);
        if (!vfs.file_exists(hook)) return;
        spdlog::info("Hooked {} with {}", normalized, hook);
        hooks.push_back(std::move(hook));
    };

    // The init file's hook directories: /schook/lua/x.lua for /lua/x.lua.
    for (const auto& dir : vfs.hook_dirs()) take(dir + normalized);

    // Each active mod's: <location><hookdir or /hook>/lua/x.lua, in order,
    // until the list's first nil.
    lua_pushstring(L, "__active_mods");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            if (lua_istable(L, -1)) {
                const std::string location = string_field(L, "location");
                std::string hookdir = string_field(L, "hookdir");
                if (hookdir.empty()) hookdir = "/hook";
                if (!location.empty()) {
                    std::string candidate = location;
                    candidate += hookdir;
                    candidate += normalized;
                    take(candidate);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return hooks;
}

Result<void> run_vfs_script(lua_State* L, std::string_view path, int env_index) {
    auto* vfs = LuaState::get_vfs(L);
    if (!vfs) return Error("doscript: VFS not initialized");

    // Relative stack indices would shift as we push; pin it.
    if (env_index < 0 && env_index > LUA_REGISTRYINDEX) {
        env_index = lua_gettop(L) + env_index + 1;
    }

    const std::string script(path);
    auto data = vfs->read_file(script);
    if (!data) return Error("doscript: file not found: " + script);

    // The script and its hooks are one chunk, as Moho loads them, so a hook
    // sees (and can replace) the script's locals.
    std::string chunk;
    append_file(chunk, *data);
    for (const std::string& hook : script_hooks(L, *vfs, script)) {
        if (auto hook_data = vfs->read_file(hook)) append_file(chunk, *hook_data);
    }

    const std::string chunk_name = "@" + script;
    if (luaL_loadbuffer(L, chunk.data(), chunk.size(), chunk_name.c_str()) != 0) {
        return Error(pop_message(L));
    }
    if (env_index != 0) {
        lua_pushvalue(L, env_index);
        lua_setfenv(L, -2);
    }
    if (lua_pcall(L, 0, 0, 0) != 0) return Error(pop_message(L));
    return {};
}

} // namespace osc::lua
