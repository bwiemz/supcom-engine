#include "renderer/emitter_blueprint.hpp"

#include "renderer/effect_blueprint_file.hpp"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace osc::renderer {

f32 EmitterCurve::peak() const {
    f32 best = -std::numeric_limits<f32>::infinity();
    for (const CurveKey& k : keys) best = std::max(best, k.z * 0.5f + k.y);
    return best;
}

namespace {

/// The blueprint's field names for its curves, in EmitterCurveId order.
constexpr std::array<const char*, kEmitterCurveCount> kCurveNames = {
    "XDirectionCurve",
    "YDirectionCurve",
    "ZDirectionCurve",
    "EmitRateCurve",
    "LifetimeCurve",
    "VelocityCurve",
    "XAccelCurve",
    "YAccelCurve",
    "ZAccelCurve",
    "ResistanceCurve",
    "SizeCurve",
    "XPosCurve",
    "YPosCurve",
    "ZPosCurve",
    "StartSizeCurve",
    "EndSizeCurve",
    "InitialRotationCurve",
    "RotationRateCurve",
    "FrameRateCurve",
    "TextureSelectionCurve",
    "RampSelectionCurve",
};

/// A boolean field (Lua true/false, or a number: non-zero), or `fallback`.
bool flag(lua_State* L, int t, const char* key, bool fallback) {
    lua_pushstring(L, key);
    lua_rawget(L, t);
    bool v = fallback;
    if (lua_type(L, -1) == LUA_TBOOLEAN) v = lua_toboolean(L, -1) != 0;
    if (lua_type(L, -1) == LUA_TNUMBER) v = lua_tonumber(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

/// A curve's keys, in order of x (make_emitter_curve_from_blueprint
/// inserts each in place).
EmitterCurve curve(lua_State* L, int t, const char* key) {
    EmitterCurve c;
    lua_pushstring(L, key);
    lua_rawget(L, t);
    const int ct = lua_gettop(L);
    if (lua_istable(L, ct)) {
        lua_pushstring(L, "Keys");
        lua_rawget(L, ct);
        const int kt = lua_gettop(L);
        if (lua_istable(L, kt)) {
            for (int i = 1;; ++i) {
                lua_rawgeti(L, kt, i);
                if (!lua_istable(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                const int k = lua_gettop(L);
                CurveKey key_value{blueprint_number(L, k, "x", 0.0f),
                                   blueprint_number(L, k, "y", 0.0f),
                                   blueprint_number(L, k, "z", 0.0f)};
                const auto at =
                    std::upper_bound(c.keys.begin(), c.keys.end(), key_value.x,
                                     [](f32 x, const CurveKey& other) { return x < other.x; });
                c.keys.insert(at, key_value);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return c;
}

EmitterBlueprintData parse(lua_State* L, int t) {
    EmitterBlueprintData bp;
    bp.lifetime = blueprint_number(L, t, "Lifetime", bp.lifetime);
    bp.repeattime = blueprint_number(L, t, "Repeattime", bp.repeattime);
    bp.frame_count = blueprint_number(L, t, "TextureFramecount", bp.frame_count);
    bp.strip_count = blueprint_number(L, t, "TextureStripcount", bp.strip_count);
    // Retail spells it both ways (Moho reflects "Blendmode").
    bp.blendmode = static_cast<i32>(blueprint_number(
        L, t, "Blendmode", blueprint_number(L, t, "BlendMode", static_cast<f32>(bp.blendmode))));
    bp.lod_cutoff = blueprint_number(L, t, "LODCutoff", bp.lod_cutoff);
    bp.sort_order = blueprint_number(L, t, "SortOrder", bp.sort_order);
    bp.local_velocity = flag(L, t, "LocalVelocity", bp.local_velocity);
    bp.local_acceleration = flag(L, t, "LocalAcceleration", bp.local_acceleration);
    bp.gravity = flag(L, t, "Gravity", bp.gravity);
    bp.align_rotation = flag(L, t, "AlignRotation", bp.align_rotation);
    bp.align_to_bone = flag(L, t, "AlignToBone", bp.align_to_bone);
    bp.flat = flag(L, t, "Flat", bp.flat);
    bp.emit_if_visible = flag(L, t, "EmitIfVisible", bp.emit_if_visible);
    bp.catchup_emit = flag(L, t, "CatchupEmit", bp.catchup_emit);
    bp.create_if_visible = flag(L, t, "CreateIfVisible", bp.create_if_visible);
    bp.particle_resistance = flag(L, t, "ParticleResistance", bp.particle_resistance);
    bp.interpolate_emission = flag(L, t, "InterpolateEmission", bp.interpolate_emission);
    bp.snap_to_waterline = flag(L, t, "SnapToWaterline", bp.snap_to_waterline);
    bp.only_emit_on_water = flag(L, t, "OnlyEmitOnWater", bp.only_emit_on_water);
    // Retail's files name them Texture and RampTexture.
    bp.texture = blueprint_path(L, t, "Texture");
    if (bp.texture.empty()) bp.texture = blueprint_path(L, t, "TextureName");
    bp.ramp_texture = blueprint_path(L, t, "RampTexture");
    if (bp.ramp_texture.empty()) bp.ramp_texture = blueprint_path(L, t, "RampTextureName");
    for (size_t i = 0; i < kCurveNames.size(); ++i) bp.curves[i] = curve(L, t, kCurveNames[i]);
    // CeilByRint of the lifetime curve's peak.
    const f32 peak = bp.curves[kLifetime].peak();
    bp.max_lifetime = std::isfinite(peak) ? static_cast<i32>(std::ceil(peak)) : 0;
    return bp;
}

} // namespace

const EmitterBlueprintData* EmitterBlueprintCache::get(const std::string& path, lua_State* L) {
    if (auto it = cache_.find(path); it != cache_.end()) return &it->second;
    if (path.empty() || failed_.count(path)) return nullptr;
    EmitterBlueprintData data;
    if (!run_effect_blueprint(vfs_, path, L, "EmitterBlueprint",
                              [&data](lua_State* S, int t) { data = parse(S, t); })) {
        failed_.insert(path);
        return nullptr;
    }
    data.blueprint_id = path;
    return &cache_.emplace(path, std::move(data)).first->second;
}

} // namespace osc::renderer
