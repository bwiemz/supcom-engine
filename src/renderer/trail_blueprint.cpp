#include "renderer/trail_blueprint.hpp"

#include "renderer/effect_blueprint_file.hpp"

#include <lua.h>

namespace osc::renderer {

namespace {

TrailBlueprintData parse(lua_State* L, int t) {
    TrailBlueprintData bp;
    bp.lifetime = blueprint_number(L, t, "Lifetime", bp.lifetime);
    bp.trail_length = blueprint_number(L, t, "TrailLength", bp.trail_length);
    bp.size = blueprint_number(L, t, "Size", bp.size);
    bp.sort_order = blueprint_number(L, t, "SortOrder", bp.sort_order);
    bp.blendmode =
        static_cast<i32>(blueprint_number(L, t, "BlendMode", static_cast<f32>(bp.blendmode)));
    if (bp.blendmode < 0 || bp.blendmode > 4) bp.blendmode = 0;
    bp.lod_cutoff = blueprint_number(L, t, "LODCutoff", bp.lod_cutoff);
    bp.fidelity = blueprint_fidelity(L, t);
    lua_pushstring(L, "EmitIfVisible");
    lua_rawget(L, t);
    if (lua_type(L, -1) == LUA_TBOOLEAN) bp.emit_if_visible = lua_toboolean(L, -1) != 0;
    if (lua_type(L, -1) == LUA_TNUMBER) bp.emit_if_visible = lua_tonumber(L, -1) != 0;
    lua_pop(L, 1);
    bp.texture_repeat_rate = blueprint_number(L, t, "TextureRepeatRate", bp.texture_repeat_rate);
    bp.repeat_texture = blueprint_path(L, t, "RepeatTexture");
    bp.ramp_texture = blueprint_path(L, t, "RampTexture");
    return bp;
}

} // namespace

const TrailBlueprintData* TrailBlueprintCache::get(const std::string& path, lua_State* L) {
    if (auto it = cache_.find(path); it != cache_.end()) return &it->second;
    if (path.empty() || failed_.count(path)) return nullptr;
    TrailBlueprintData data;
    if (!run_effect_blueprint(vfs_, path, L, "TrailEmitterBlueprint",
                              [&data](lua_State* S, int t) { data = parse(S, t); })) {
        failed_.insert(path);
        return nullptr;
    }
    return &cache_.emplace(path, std::move(data)).first->second;
}

} // namespace osc::renderer
