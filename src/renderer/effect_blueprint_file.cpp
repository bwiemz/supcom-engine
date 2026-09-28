#include "renderer/effect_blueprint_file.hpp"

#include "vfs/virtual_file_system.hpp"

#include <lauxlib.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cctype>
#include <cstring>

namespace osc::renderer {

namespace {

/// Registry key the capturing global stores its argument under.
constexpr const char* kCaptureKey = "__osc_effect_bp_capture";

/// The globals effect blueprint files call.
constexpr std::array<const char*, 3> kKinds = {"EmitterBlueprint", "BeamBlueprint",
                                               "TrailEmitterBlueprint"};

int capture_blueprint(lua_State* L) {
    lua_pushstring(L, kCaptureKey);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

int ignore_blueprint(lua_State*) {
    return 0;
}

} // namespace

bool run_effect_blueprint(const vfs::VirtualFileSystem* vfs, const std::string& path, lua_State* L,
                          const char* kind, const std::function<void(lua_State*, int)>& parse) {
    if (!vfs || !L) return false;
    const auto content = vfs->read_file(path);
    if (!content) return false;

    const int top = lua_gettop(L);
    // Keep each kind's global (at top + 1 …), then swap in the run's.
    for (const char* k : kKinds) {
        lua_pushstring(L, k);
        lua_rawget(L, LUA_GLOBALSINDEX);
    }
    for (const char* k : kKinds) {
        lua_pushstring(L, k);
        lua_pushcfunction(L, std::strcmp(k, kind) == 0 ? capture_blueprint : ignore_blueprint);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }

    const std::string chunk = "@" + path;
    bool ok = luaL_loadbuffer(L, content->data(), content->size(), chunk.c_str()) == 0 &&
              lua_pcall(L, 0, 0, 0) == 0;
    if (!ok) spdlog::debug("{}: failed to load {}: {}", kind, path, lua_tostring(L, -1));

    for (size_t i = 0; i < kKinds.size(); ++i) {
        lua_pushstring(L, kKinds[i]);
        lua_pushvalue(L, top + 1 + static_cast<int>(i));
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
    lua_pushstring(L, kCaptureKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    ok = ok && lua_istable(L, -1);
    if (ok) parse(L, lua_gettop(L));
    lua_pushstring(L, kCaptureKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
    return ok;
}

f32 blueprint_number(lua_State* L, int idx, const char* key, f32 fallback) {
    lua_pushstring(L, key);
    lua_rawget(L, idx);
    const f32 v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

std::string blueprint_path(lua_State* L, int idx, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, idx);
    std::string out;
    if (lua_type(L, -1) == LUA_TSTRING) {
        out = lua_tostring(L, -1);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    lua_pop(L, 1);
    return out;
}

} // namespace osc::renderer
