#include "renderer/emitter_blueprint.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace osc::renderer {

// ---------------------------------------------------------------------------
// EmitterCurve
// ---------------------------------------------------------------------------

f32 EmitterCurve::sample(f32 t) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) return keys[0].y;

    // Clamp t to [0, x_range]
    t = std::clamp(t, 0.0f, x_range);

    // Find the two keys surrounding t
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t <= keys[i + 1].x) {
            f32 span = keys[i + 1].x - keys[i].x;
            if (span <= 0.0f) return keys[i].y;
            f32 frac = (t - keys[i].x) / span;
            return keys[i].y + frac * (keys[i + 1].y - keys[i].y);
        }
    }
    return keys.back().y;
}

namespace {

/// The renderer's own random stream for particle variance, apart from the
/// sim's (and from the C library's rand(), which the UI state's math.random
/// shares). Seeded the same every run, so captures are reproducible.
f32 visual_random01() {
    thread_local u64 state = 0x2545F4914F6CDD1Dull;
    u64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<f32>(z >> 40) * (1.0f / 16777216.0f);
}

} // namespace

f32 EmitterCurve::sample_random(f32 t) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) {
        f32 variance = keys[0].z;
        f32 r = visual_random01();
        return keys[0].y + variance * (2.0f * r - 1.0f);
    }

    t = std::clamp(t, 0.0f, x_range);

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t <= keys[i + 1].x) {
            f32 span = keys[i + 1].x - keys[i].x;
            f32 frac = (span > 0.0f) ? (t - keys[i].x) / span : 0.0f;
            f32 y = keys[i].y + frac * (keys[i + 1].y - keys[i].y);
            f32 z = keys[i].z + frac * (keys[i + 1].z - keys[i].z);
            f32 r = visual_random01();
            return y + z * (2.0f * r - 1.0f);
        }
    }

    f32 variance = keys.back().z;
    f32 r = visual_random01();
    return keys.back().y + variance * (2.0f * r - 1.0f);
}

// ---------------------------------------------------------------------------
// Lua field helpers
// ---------------------------------------------------------------------------

static f32 lua_field_f32(lua_State* L, int table_idx, const char* name,
                         f32 fallback = 0.0f) {
    lua_pushstring(L, name);
    lua_rawget(L, table_idx);
    f32 val = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1))
                                  : fallback;
    lua_pop(L, 1);
    return val;
}

static bool lua_field_bool(lua_State* L, int table_idx, const char* name,
                           bool fallback = false) {
    lua_pushstring(L, name);
    lua_rawget(L, table_idx);
    bool val = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) != 0) : fallback;
    lua_pop(L, 1);
    return val;
}

static std::string lua_field_string(lua_State* L, int table_idx,
                                    const char* name,
                                    const char* fallback = "") {
    lua_pushstring(L, name);
    lua_rawget(L, table_idx);
    std::string val;
    if (lua_type(L, -1) == LUA_TSTRING) {
        val = lua_tostring(L, -1);
    } else {
        val = fallback;
    }
    lua_pop(L, 1);
    return val;
}

static u32 lua_field_u32(lua_State* L, int table_idx, const char* name,
                         u32 fallback = 0) {
    lua_pushstring(L, name);
    lua_rawget(L, table_idx);
    u32 val = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1))
                                  : fallback;
    lua_pop(L, 1);
    return val;
}

// ---------------------------------------------------------------------------
// parse_curve
// ---------------------------------------------------------------------------

EmitterCurve EmitterBlueprintCache::parse_curve(lua_State* L, int table_idx,
                                                const char* field_name) {
    EmitterCurve curve;

    lua_pushstring(L, field_name);
    lua_rawget(L, table_idx);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return curve;
    }
    int curve_idx = lua_gettop(L);

    curve.x_range = lua_field_f32(L, curve_idx, "XRange", 1.0f);

    // Read Keys array
    lua_pushstring(L, "Keys");
    lua_rawget(L, curve_idx);
    if (lua_istable(L, -1)) {
        int keys_idx = lua_gettop(L);
        int n = luaL_getn(L, keys_idx);
        curve.keys.reserve(static_cast<size_t>(n));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, keys_idx, i);
            if (lua_istable(L, -1)) {
                int key_idx = lua_gettop(L);
                CurveKey k;
                k.x = lua_field_f32(L, key_idx, "x");
                k.y = lua_field_f32(L, key_idx, "y");
                k.z = lua_field_f32(L, key_idx, "z");
                curve.keys.push_back(k);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // Keys

    lua_pop(L, 1); // curve table
    return curve;
}

// ---------------------------------------------------------------------------
// parse_from_lua
// ---------------------------------------------------------------------------

EmitterBlueprintData EmitterBlueprintCache::parse_from_lua(lua_State* L,
                                                           int table_idx) {
    EmitterBlueprintData bp;

    // Scalar fields
    bp.lifetime = lua_field_f32(L, table_idx, "Lifetime", 1.0f);
    bp.repeattime = lua_field_f32(L, table_idx, "RepeatTime", 1.0f);
    bp.texture_frame_count =
        lua_field_u32(L, table_idx, "TextureFrameCount", 1);
    bp.texture_strip_count =
        lua_field_u32(L, table_idx, "TextureStripCount", 1);
    bp.blendmode = lua_field_u32(L, table_idx, "Blendmode", 0);
    bp.lod_cutoff = lua_field_f32(L, table_idx, "LODCutoff", 300.0f);
    bp.sort_order = lua_field_f32(L, table_idx, "SortOrder", 0.0f);

    // Boolean flags
    bp.local_velocity =
        lua_field_bool(L, table_idx, "LocalVelocity", false);
    bp.local_acceleration =
        lua_field_bool(L, table_idx, "LocalAcceleration", false);
    bp.gravity = lua_field_bool(L, table_idx, "Gravity", false);
    bp.align_rotation =
        lua_field_bool(L, table_idx, "AlignRotation", false);
    bp.align_to_bone = lua_field_bool(L, table_idx, "AlignToBone", false);
    bp.flat = lua_field_bool(L, table_idx, "Flat", false);
    bp.emit_if_visible =
        lua_field_bool(L, table_idx, "EmitIfVisible", true);
    bp.catchup_emit = lua_field_bool(L, table_idx, "CatchupEmit", true);
    bp.snap_to_waterline =
        lua_field_bool(L, table_idx, "SnapToWaterline", false);
    bp.only_emit_on_water =
        lua_field_bool(L, table_idx, "OnlyEmitOnWater", false);
    bp.interpolate_emission =
        lua_field_bool(L, table_idx, "InterpolateEmission", true);

    // Texture paths
    bp.texture_path =
        lua_field_string(L, table_idx, "TextureFileName", "");
    bp.ramp_texture_path =
        lua_field_string(L, table_idx, "RampTexture", "");

    // Emitter-level curves
    bp.emit_rate = parse_curve(L, table_idx, "EmitRateCurve");
    bp.x_direction = parse_curve(L, table_idx, "XDirectionCurve");
    bp.y_direction = parse_curve(L, table_idx, "YDirectionCurve");
    bp.z_direction = parse_curve(L, table_idx, "ZDirectionCurve");
    bp.velocity = parse_curve(L, table_idx, "VelocityCurve");
    bp.x_accel = parse_curve(L, table_idx, "XAccelCurve");
    bp.y_accel = parse_curve(L, table_idx, "YAccelCurve");
    bp.z_accel = parse_curve(L, table_idx, "ZAccelCurve");
    bp.x_pos = parse_curve(L, table_idx, "XPosCurve");
    bp.y_pos = parse_curve(L, table_idx, "YPosCurve");
    bp.z_pos = parse_curve(L, table_idx, "ZPosCurve");

    // Per-particle curves
    bp.lifetime_curve = parse_curve(L, table_idx, "LifetimeCurve");
    bp.start_size = parse_curve(L, table_idx, "StartSizeCurve");
    bp.end_size = parse_curve(L, table_idx, "EndSizeCurve");
    bp.size_curve = parse_curve(L, table_idx, "SizeCurve");
    bp.initial_rotation =
        parse_curve(L, table_idx, "InitialRotationCurve");
    bp.rotation_rate = parse_curve(L, table_idx, "RotationRateCurve");
    bp.frame_rate = parse_curve(L, table_idx, "FrameRateCurve");
    bp.texture_selection =
        parse_curve(L, table_idx, "TextureSelectionCurve");
    bp.ramp_selection = parse_curve(L, table_idx, "RampSelectionCurve");

    return bp;
}

// ---------------------------------------------------------------------------
// get — load & cache
// ---------------------------------------------------------------------------

const EmitterBlueprintData*
EmitterBlueprintCache::get(const std::string& bp_path, lua_State* L) {
    if (auto it = cache_.find(bp_path); it != cache_.end()) return &it->second;
    if (failed_.count(bp_path)) return nullptr;

    EmitterBlueprintData data;
    if (!load(bp_path, L, data)) {
        failed_.insert(bp_path);
        return nullptr;
    }
    data.blueprint_id = bp_path;
    auto [ins, _] = cache_.emplace(bp_path, std::move(data));
    return &ins->second;
}

namespace {
/// Registry key the capturing EmitterBlueprint stores its argument under.
constexpr const char* kCaptureKey = "__osc_emitter_bp_capture";

int capture_emitter_blueprint(lua_State* L) {
    lua_pushstring(L, kCaptureKey);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}
} // namespace

bool EmitterBlueprintCache::load(const std::string& bp_path, lua_State* L,
                                 EmitterBlueprintData& out) const {
    if (!vfs_ || !L) return false;
    auto content = vfs_->read_file(bp_path);
    if (!content) {
        spdlog::warn("EmitterBlueprint: {} not found", bp_path);
        return false;
    }

    const int top = lua_gettop(L);
    // Swap in a capturing EmitterBlueprint for the duration of the run.
    lua_pushstring(L, "EmitterBlueprint");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int saved = lua_gettop(L);
    lua_pushstring(L, "EmitterBlueprint");
    lua_pushcfunction(L, capture_emitter_blueprint);
    lua_rawset(L, LUA_GLOBALSINDEX);

    const std::string chunk = "@" + bp_path;
    bool ok = luaL_loadbuffer(L, content->data(), content->size(), chunk.c_str()) == 0 &&
              lua_pcall(L, 0, 0, 0) == 0;
    if (!ok) spdlog::warn("EmitterBlueprint: failed to load {}: {}", bp_path,
                          lua_tostring(L, -1));

    // Restore the previous global and take the captured table.
    lua_pushstring(L, "EmitterBlueprint");
    lua_pushvalue(L, saved);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, kCaptureKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (ok && !lua_istable(L, -1)) {
        spdlog::warn("EmitterBlueprint: {} defined no emitter", bp_path);
        ok = false;
    }
    if (ok) out = parse_from_lua(L, lua_gettop(L));
    lua_pushstring(L, kCaptureKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
    return ok;
}

} // namespace osc::renderer
