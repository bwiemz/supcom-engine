#include "lua/lua_state.hpp"
#include "vfs/virtual_file_system.hpp"
#include "blueprints/blueprint_store.hpp"

#include <fstream>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

namespace osc::lua {

LuaState::LuaState() {
    L_ = lua_open();
    if (!L_) {
        spdlog::error("Failed to create Lua state");
        return;
    }

    // Open standard libraries
    luaopen_base(L_);
    luaopen_table(L_);
    luaopen_io(L_);
    luaopen_string(L_);
    luaopen_math(L_);
    luaopen_debug(L_);

    // LuaPlus compatibility: create per-type metatables for primitive types.
    // LuaPlus allows setting attributes on nil/bool/number/string; config.lua
    // expects getmetatable() on these to return a table (so it can lock them).
    lua_pushnil(L_);
    lua_newtable(L_);
    lua_setmetatable(L_, -2);
    lua_pop(L_, 1);

    lua_pushboolean(L_, 0);
    lua_newtable(L_);
    lua_setmetatable(L_, -2);
    lua_pop(L_, 1);

    lua_pushnumber(L_, 0);
    lua_newtable(L_);
    lua_setmetatable(L_, -2);
    lua_pop(L_, 1);

    // String metatable: __index = string table (enables "str":method() syntax)
    lua_pushstring(L_, "");
    lua_newtable(L_);
    lua_pushstring(L_, "__index");
    lua_getglobal(L_, "string");
    lua_settable(L_, -3);
    lua_setmetatable(L_, -2);
    lua_pop(L_, 1);

    // Thread type metatable (config.lua line 38 expects this)
    lua_newthread(L_);
    lua_newtable(L_);
    lua_setmetatable(L_, -2);
    lua_pop(L_, 1);
}

LuaState::~LuaState() {
    if (L_) {
        lua_close(L_);
    }
}

LuaState::LuaState(LuaState&& other) noexcept : L_(other.L_) {
    other.L_ = nullptr;
}

LuaState& LuaState::operator=(LuaState&& other) noexcept {
    if (this != &other) {
        if (L_) lua_close(L_);
        L_ = other.L_;
        other.L_ = nullptr;
    }
    return *this;
}

void LuaState::register_function(const char* name, int (*fn)(lua_State*)) {
    lua_register(L_, name, fn);
}

void LuaState::register_table_function(const char* table_name,
                                         const char* func_name,
                                         int (*fn)(lua_State*)) {
    lua_getglobal(L_, table_name);
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_setglobal(L_, table_name);
        lua_getglobal(L_, table_name);
    }
    lua_pushstring(L_, func_name);
    lua_pushcfunction(L_, fn);
    lua_settable(L_, -3);
    lua_pop(L_, 1);
}

void LuaState::set_global_string(const char* name, const char* value) {
    lua_pushstring(L_, value);
    lua_setglobal(L_, name);
}

void LuaState::set_global_bool(const char* name, bool value) {
    lua_pushboolean(L_, value ? 1 : 0);
    lua_setglobal(L_, name);
}

void LuaState::set_global_table(const char* name) {
    lua_newtable(L_);
    lua_setglobal(L_, name);
}

Result<void> LuaState::do_string(std::string_view code) {
    int status = luaL_loadbuffer(L_, code.data(), code.size(), "=string");
    if (status != 0) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return Error(std::move(err));
    }

    status = lua_pcall(L_, 0, 0, 0);
    if (status != 0) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return Error(std::move(err));
    }

    return {};
}

Result<void> LuaState::do_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return Error("Failed to open file: " + path.string());
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(static_cast<size_t>(size));
    if (!file.read(buffer.data(), size)) {
        return Error("Failed to read file: " + path.string());
    }

    return do_buffer(buffer.data(), buffer.size(),
                     ("@" + path.string()).c_str());
}

Result<void> LuaState::do_buffer(const char* buf, size_t len,
                                   const char* name) {
    // Strip UTF-8 BOM if present
    if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf += 3;
        len -= 3;
    }

    int status = luaL_loadbuffer(L_, buf, len, name);
    if (status != 0) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return Error(std::move(err));
    }

    status = lua_pcall(L_, 0, 0, 0);
    if (status != 0) {
        std::string err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return Error(std::move(err));
    }

    return {};
}

void LuaState::set_vfs(vfs::VirtualFileSystem* vfs) {
    lua_pushlightuserdata(L_, vfs);
    lua_setglobal(L_, REG_VFS);
}

void LuaState::set_blueprint_store(blueprints::BlueprintStore* store) {
    lua_pushlightuserdata(L_, store);
    lua_setglobal(L_, REG_BLUEPRINT_STORE);
}

vfs::VirtualFileSystem* LuaState::get_vfs(lua_State* L) {
    lua_getglobal(L, REG_VFS);
    auto* vfs = static_cast<vfs::VirtualFileSystem*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return vfs;
}

blueprints::BlueprintStore* LuaState::get_blueprint_store(lua_State* L) {
    lua_getglobal(L, REG_BLUEPRINT_STORE);
    auto* store = static_cast<blueprints::BlueprintStore*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return store;
}

} // namespace osc::lua
