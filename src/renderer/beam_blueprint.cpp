#include "renderer/beam_blueprint.hpp"

#include "renderer/effect_blueprint_file.hpp"

#include <lua.h>

namespace osc::renderer {

namespace {

/// A colour table {x=, y=, z=, w=} (R, G, B, A); what it leaves out stays.
void color_field(lua_State* L, int idx, const char* key, std::array<f32, 4>& out) {
    lua_pushstring(L, key);
    lua_rawget(L, idx);
    const int t = lua_gettop(L);
    if (lua_istable(L, t)) {
        out[0] = blueprint_number(L, t, "x", out[0]);
        out[1] = blueprint_number(L, t, "y", out[1]);
        out[2] = blueprint_number(L, t, "z", out[2]);
        out[3] = blueprint_number(L, t, "w", out[3]);
    }
    lua_pop(L, 1);
}

BeamBlueprintData parse(lua_State* L, int t) {
    BeamBlueprintData bp;
    bp.texture = blueprint_path(L, t, "TextureName");
    bp.length = blueprint_number(L, t, "Length", bp.length);
    bp.lifetime = blueprint_number(L, t, "Lifetime", bp.lifetime);
    bp.thickness = blueprint_number(L, t, "Thickness", bp.thickness);
    bp.ushift = blueprint_number(L, t, "UShift", bp.ushift);
    bp.vshift = blueprint_number(L, t, "VShift", bp.vshift);
    color_field(L, t, "StartColor", bp.start_color);
    color_field(L, t, "EndColor", bp.end_color);
    bp.lod_cutoff = blueprint_number(L, t, "LODCutoff", bp.lod_cutoff);
    bp.fidelity = blueprint_fidelity(L, t);
    bp.repeat_rate = blueprint_number(L, t, "RepeatRate", bp.repeat_rate);
    bp.blendmode =
        static_cast<i32>(blueprint_number(L, t, "Blendmode", static_cast<f32>(bp.blendmode)));
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
    return run_effect_blueprint(vfs_, path, L, "BeamBlueprint",
                                [&out](lua_State* S, int t) { out = parse(S, t); });
}

} // namespace osc::renderer
