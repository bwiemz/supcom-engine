#include "renderer/beam_blueprint.hpp"

#include "vfs/virtual_file_system.hpp"

#include <lauxlib.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <cctype>

namespace osc::renderer {

namespace {

/// Registry key the capturing BeamBlueprint stores its argument under.
constexpr const char* kCaptureKey = "__osc_beam_bp_capture";

int capture_beam_blueprint(lua_State* L) {
    lua_pushstring(L, kCaptureKey);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// t[key] of the table at absolute index `idx`, pushed.
void push_field(lua_State* L, int idx, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, idx);
}

f32 number_field(lua_State* L, int idx, const char* key, f32 fallback) {
    push_field(L, idx, key);
    const f32 v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

/// A colour table {x=, y=, z=, w=} (R, G, B, A); what it leaves out stays.
void color_field(lua_State* L, int idx, const char* key, std::array<f32, 4>& out) {
    push_field(L, idx, key);
    const int t = lua_gettop(L);
    if (lua_istable(L, t)) {
        out[0] = number_field(L, t, "x", out[0]);
        out[1] = number_field(L, t, "y", out[1]);
        out[2] = number_field(L, t, "z", out[2]);
        out[3] = number_field(L, t, "w", out[3]);
    }
    lua_pop(L, 1);
}

BeamBlueprintData parse(lua_State* L, int t) {
    BeamBlueprintData bp;
    push_field(L, t, "TextureName");
    if (lua_type(L, -1) == LUA_TSTRING) {
        bp.texture = lua_tostring(L, -1);
        for (char& c : bp.texture)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    lua_pop(L, 1);
    bp.length = number_field(L, t, "Length", bp.length);
    bp.lifetime = number_field(L, t, "Lifetime", bp.lifetime);
    bp.thickness = number_field(L, t, "Thickness", bp.thickness);
    bp.ushift = number_field(L, t, "UShift", bp.ushift);
    bp.vshift = number_field(L, t, "VShift", bp.vshift);
    color_field(L, t, "StartColor", bp.start_color);
    color_field(L, t, "EndColor", bp.end_color);
    bp.lod_cutoff = number_field(L, t, "LODCutoff", bp.lod_cutoff);
    bp.repeat_rate = number_field(L, t, "RepeatRate", bp.repeat_rate);
    bp.blendmode =
        static_cast<i32>(number_field(L, t, "Blendmode", static_cast<f32>(bp.blendmode)));
    if (bp.blendmode < 0 || bp.blendmode > 4) bp.blendmode = 3;
    return bp;
}

} // namespace

const BeamBlueprintData* BeamBlueprintCache::get(const std::string& path, lua_State* L) {
    if (auto it = cache_.find(path); it != cache_.end()) return &it->second;
    if (path.empty() || failed_.count(path)) return nullptr;
    BeamBlueprintData data;
    if (!load(path, L, data)) {
        failed_.insert(path);
        return nullptr;
    }
    return &cache_.emplace(path, std::move(data)).first->second;
}

bool BeamBlueprintCache::load(const std::string& path, lua_State* L, BeamBlueprintData& out) const {
    if (!vfs_ || !L) return false;
    const auto content = vfs_->read_file(path);
    if (!content) return false;

    const int top = lua_gettop(L);
    // Swap in a capturing BeamBlueprint (and a silent EmitterBlueprint: an
    // emitter's file is simply not a beam's) for the run.
    lua_pushstring(L, "BeamBlueprint");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int saved_beam = lua_gettop(L);
    lua_pushstring(L, "EmitterBlueprint");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int saved_emitter = lua_gettop(L);
    lua_pushstring(L, "BeamBlueprint");
    lua_pushcfunction(L, capture_beam_blueprint);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "EmitterBlueprint");
    lua_pushcfunction(L, [](lua_State*) { return 0; });
    lua_rawset(L, LUA_GLOBALSINDEX);

    const std::string chunk = "@" + path;
    bool ok = luaL_loadbuffer(L, content->data(), content->size(), chunk.c_str()) == 0 &&
              lua_pcall(L, 0, 0, 0) == 0;
    if (!ok) spdlog::debug("BeamBlueprint: failed to load {}: {}", path, lua_tostring(L, -1));

    lua_pushstring(L, "BeamBlueprint");
    lua_pushvalue(L, saved_beam);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "EmitterBlueprint");
    lua_pushvalue(L, saved_emitter);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, kCaptureKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    ok = ok && lua_istable(L, -1);
    if (ok) out = parse(L, lua_gettop(L));
    lua_pushstring(L, kCaptureKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
    return ok;
}

} // namespace osc::renderer
