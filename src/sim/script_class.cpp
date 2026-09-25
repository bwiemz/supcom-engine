#include "sim/script_class.hpp"

extern "C" {
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace osc::sim {

namespace {

/// A string field of the table at `index` ("" if absent).
std::string string_field(lua_State* L, int index, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, index);
    std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return value;
}

/// Resolve without the cache: pushes the class table, or nil.
void resolve(lua_State* L, const std::string& bp_id, std::string_view bp_suffix, const char* kind,
             bool warn_default_missing) {
    const int top = lua_gettop(L);
    std::string module, class_name;
    bool default_module = false;
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp_id.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            const int bp = lua_gettop(L);
            module = string_field(L, bp, "ScriptModule");
            class_name = string_field(L, bp, "ScriptClass");
            if (module.empty()) {
                module = default_script_module(string_field(L, bp, "Source"), bp_suffix);
                default_module = true;
            }
        }
    }
    lua_settop(L, top);
    if (class_name.empty()) class_name = "TypeClass";

    // A default module (beside the .bp) often isn't there -- most props have
    // none -- so ask the VFS before import logs a missing file.
    if (default_module && !module.empty()) {
        lua_pushstring(L, "exists");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            lua_pushstring(L, module.c_str());
            const bool there = lua_pcall(L, 1, 1, 0) == 0 && lua_toboolean(L, -1);
            lua_settop(L, top);
            if (!there) {
                lua_pushnil(L);
                return;
            }
        } else {
            lua_settop(L, top);
        }
    }

    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (module.empty() || !lua_isfunction(L, -1)) {
        lua_settop(L, top);
        lua_pushnil(L);
        return;
    }
    lua_pushstring(L, module.c_str());
    if (lua_pcall(L, 1, 1, 0) != 0) {
        if (!default_module || warn_default_missing)
            spdlog::warn("{} script {} ({}) failed to load: {}", kind, module, bp_id,
                         lua_tostring(L, -1));
        lua_settop(L, top);
        lua_pushnil(L);
        return;
    }
    if (lua_istable(L, -1)) {
        lua_pushstring(L, class_name.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_remove(L, -2); // module table
            return;
        }
    }
    spdlog::warn("{} script {} ({}) defines no {}", kind, module, bp_id, class_name);
    lua_settop(L, top);
    lua_pushnil(L);
}

} // namespace

std::string default_script_module(std::string source, std::string_view bp_suffix) {
    std::transform(source.begin(), source.end(), source.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (source.size() <= bp_suffix.size() ||
        source.compare(source.size() - bp_suffix.size(), bp_suffix.size(), bp_suffix) != 0)
        return {};
    return source.substr(0, source.size() - bp_suffix.size()) + "_script.lua";
}

void push_blueprint_script_class(lua_State* L, const std::string& bp_id, std::string_view bp_suffix,
                                 const char* cache_key, const char* kind,
                                 bool warn_default_missing) {
    lua_pushstring(L, cache_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, cache_key);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    const int cache = lua_gettop(L);
    lua_pushstring(L, bp_id.c_str());
    lua_rawget(L, cache);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        resolve(L, bp_id, bp_suffix, kind, warn_default_missing);
        // Cache a miss as false, so a broken script is tried (and logged) once.
        lua_pushstring(L, bp_id.c_str());
        if (lua_istable(L, -2)) lua_pushvalue(L, -2);
        else lua_pushboolean(L, 0);
        lua_rawset(L, cache);
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    lua_remove(L, cache);
}

void push_new_script_object(lua_State* L, const char* kind) {
    const int cls = lua_gettop(L);
    if (!lua_istable(L, cls)) {
        lua_pop(L, 1);
        lua_newtable(L);
        return;
    }
    if (lua_getmetatable(L, cls)) {
        lua_pushstring(L, "__call");
        lua_gettable(L, -2);
        lua_remove(L, -2); // the class's metatable
        if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, cls);
            if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
                lua_remove(L, cls);
                return;
            }
            spdlog::warn("{} script object: {}", kind,
                         lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                                        : "its class returned no table");
        }
        lua_pop(L, 1); // the error, the non-table, or nil
    }
    lua_newtable(L);
    lua_pushvalue(L, cls);
    lua_setmetatable(L, -2);
    lua_remove(L, cls);
}

} // namespace osc::sim
